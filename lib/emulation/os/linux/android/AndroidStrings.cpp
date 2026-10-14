//===- AndroidStrings.cpp - Bounded guest string operations --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>

namespace neverd::emulation::android_model {
BionicResult Bionic::decimalInteger(const NativeCallEvent &Call) {
  const bool Narrow = Call.Name == symbol::Atoi;
  auto Parsed =
      decimalValue(Narrow, [&](uint64_t I) -> llvm::Expected<uint8_t> {
        if (Call.Arguments[0] > UINT64_MAX - I)
          return failure(diagnostic::StringAddressOverflow);
        return byte(Call.Arguments[0] + I);
      });
  if (!Parsed)
    return Parsed.takeError();
  return *Parsed ? value(**Parsed) : std::optional<BionicValue>();
}

llvm::Expected<std::optional<uint64_t>> Bionic::decimalValue(
    bool Narrow,
    llvm::function_ref<llvm::Expected<uint8_t>(uint64_t)> ReadByte) {
  const uint64_t PositiveLimit = Narrow ? INT32_MAX : INT64_MAX;
  enum class Phase { Space, Sign, Digits };
  Phase P = Phase::Space;
  bool Negative = false;
  uint64_t Number = 0;
  for (uint64_t I = 0; I < Options.MemoryLimit; ++I) {
    auto C = ReadByte(I);
    if (!C)
      return C.takeError();
    if (P == Phase::Space) {
      if (*C == ' ' || (*C >= '\t' && *C <= '\r'))
        continue;
      P = Phase::Sign;
    }
    if (P == Phase::Sign) {
      P = Phase::Digits;
      if (*C == '+' || *C == '-') {
        Negative = *C == '-';
        continue;
      }
    }
    if (*C < '0' || *C > '9') {
      const uint64_t Value = Negative ? uint64_t(0) - Number : Number;
      return std::optional<uint64_t>(Narrow ? uint32_t(Value) : Value);
    }
    const unsigned Digit = *C - '0';
    const uint64_t Limit = PositiveLimit + uint64_t(Negative);
    if (Number > (Limit - Digit) / 10) {
      // Only defined, representable conversions are admitted. Do not choose
      // libc-specific overflow, narrowing or errno behavior for the caller.
      Result.Stop = ProcessStopReason::UnsupportedService;
      Result.Diagnostic = diagnostic::DecimalRange;
      return std::optional<uint64_t>();
    }
    Number = Number * 10 + Digit;
  }
  return failure(diagnostic::StringScanLimit);
}

BionicResult Bionic::findCharacter(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  llvm::StringRef Name(Call.Name);
  bool Fortified = Name == symbol::StrchrChk || Name == symbol::StrrchrChk;
  bool FindLast = Name == symbol::Strrchr || Name == symbol::StrrchrChk;
  uint8_t Character = static_cast<uint8_t>(A[1]);
  uint64_t Bound = Fortified ? A[2] : Options.MemoryLimit;
  uint64_t Match = 0;
  // Check the object extent before each read. A first match may return without
  // reaching NUL; a last match still requires a complete terminated string.
  for (uint64_t I = 0, Limit = std::min(Bound, Options.MemoryLimit); I < Limit;
       ++I) {
    if (A[0] > UINT64_MAX - I)
      return failure(diagnostic::StringAddressOverflow);
    uint64_t Address = A[0] + I;
    auto C = byte(Address);
    if (!C)
      return C.takeError();
    if (*C == Character) {
      if (!FindLast)
        return value(Address);
      Match = Address;
    }
    if (!*C)
      return value(Match);
  }
  if (Fortified && Bound <= Options.MemoryLimit)
    return failure(diagnostic::FortifyReadPrefix + Name);
  return failure(diagnostic::StringScanLimit);
}

BionicResult Bionic::tokenize(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  uint64_t Input = A[0];
  if (A[2] % 8)
    return failure(diagnostic::TokenCursorAlignment);
  if (!Input) {
    if (auto E = access(A[2], 8, Read))
      return std::move(E);
    uint8_t Saved[8];
    if (auto E = CPU.read(A[2], Saved))
      return std::move(E);
    Input = llvm::support::endian::read64le(Saved);
    // API 28 leaves an exhausted context untouched, without reading delim.
    if (!Input)
      return value(0);
  }
  auto Delimiters = string(A[1]);
  if (!Delimiters)
    return Delimiters.takeError();
  std::array<bool, 256> Separators{};
  for (unsigned char C : *Delimiters)
    Separators[C] = true;

  std::optional<uint64_t> Token;
  for (uint64_t I = 0; I < Options.MemoryLimit; ++I) {
    if (Input > UINT64_MAX - I)
      return failure(diagnostic::TokenAddressOverflow);
    uint64_t Address = Input + I;
    auto C = byte(Address);
    if (!C)
      return C.takeError();
    bool Split = Token && *C && Separators[*C];
    if (!*C || Split) {
      if (Split && Address == UINT64_MAX)
        return failure(diagnostic::TokenContinuationOverflow);
      // Validate both effects before terminating the token. Only the delimiter
      // byte needs write access; an unseparated final token can be read-only.
      if (auto E = access(A[2], 8, Write))
        return std::move(E);
      if (Split)
        if (auto E = access(Address, 1, Write))
          return std::move(E);
      uint8_t Saved[8], Zero = 0;
      llvm::support::endian::write64le(Saved, Split ? Address + 1 : 0);
      if (Split)
        if (auto E = CPU.write(Address, llvm::ArrayRef<uint8_t>(&Zero, 1)))
          return std::move(E);
      if (auto E = CPU.write(A[2], Saved))
        return std::move(E);
      return value(Token.value_or(0));
    }
    if (!Separators[*C] && !Token)
      Token = Address;
  }
  return failure(diagnostic::TokenScanLimit);
}
llvm::Expected<std::string> Bionic::string(uint64_t Address) {
  std::string Text;
  for (uint64_t I = 0; I < Options.MemoryLimit && Address <= UINT64_MAX - I;
       ++I) {
    auto V = byte(Address + I);
    if (!V)
      return V.takeError();
    if (!*V)
      return Text;
    Text.push_back(*V);
  }
  return failure(diagnostic::UnterminatedString);
}
BionicResult Bionic::stringLength(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  llvm::StringRef Name(Call.Name);
  uint64_t Bound = Name == symbol::Strnlen ? std::min(A[1], Options.MemoryLimit)
                                           : Options.MemoryLimit;
  for (uint64_t I = 0; I < Bound; ++I) {
    if (A[0] > UINT64_MAX - I)
      return failure(diagnostic::StringAddressOverflow);
    auto V = byte(A[0] + I);
    if (!V)
      return V.takeError();
    if (!*V)
      return value(I);
  }
  if (Name == symbol::Strnlen && Bound == A[1])
    return value(Bound);
  return failure(diagnostic::StringScanLimit);
}

BionicResult Bionic::compareString(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  llvm::StringRef Name(Call.Name);
  uint64_t Bound = Name == symbol::Strncmp ? std::min(A[2], Options.MemoryLimit)
                                           : Options.MemoryLimit;
  for (uint64_t I = 0; I < Bound; ++I) {
    if (A[0] > UINT64_MAX - I || A[1] > UINT64_MAX - I)
      return failure(diagnostic::StringAddressOverflow);
    auto Left = byte(A[0] + I);
    if (!Left)
      return Left.takeError();
    auto Right = byte(A[1] + I);
    if (!Right)
      return Right.takeError();
    if (*Left != *Right)
      return value(static_cast<uint32_t>(int(*Left) - int(*Right)));
    if (!*Left)
      return value(0);
  }
  if (Name == symbol::Strncmp && Bound == A[2])
    return value(0);
  return failure(diagnostic::StringCompareLimit);
}

} // namespace neverd::emulation::android_model
