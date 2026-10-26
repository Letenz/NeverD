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
BionicResult Bionic::copyBoundedString(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  const uint64_t Count = A[2];
  llvm::StringRef Name(Call.Name);
  const bool Complete = Name == symbol::Strcpy || Name == symbol::StrcpyChk;
  if ((Name == symbol::StrncpyChk || Name == symbol::StrncpyChk2 ||
       Name == symbol::StrlcpyChk) &&
      Count > A[3])
    return failure(diagnostic::FortifyWritePrefix + Name);
  if (Complete || Name == symbol::Strlcpy || Name == symbol::StrlcpyChk) {
    // strlcpy returns the complete source length even when no bytes fit.
    // Stage the terminated source before publishing either copy's effect.
    auto Text = string(A[1]);
    if (!Text)
      return Text.takeError();
    const uint64_t Length = Text->size();
    // API 28 ARM64 checks strlen(src) + 1 before accessing the destination.
    // Source faults and scan budgets therefore precede fortify refusal.
    if (Name == symbol::StrcpyChk && Length >= Count)
      return failure(diagnostic::FortifyWritePrefix + Name);
    if (!Complete && !Count)
      return value(Length);
    const uint64_t Copied = Complete ? Length : std::min(Length, Count - 1);
    const uint64_t Written = Copied + 1;
    // Only the actual write extent is required; capacity does not imply
    // padding or reads beyond the source terminator. All source bytes read,
    // including that terminator, participate in undefined-overlap rejection.
    if (A[0] <= A[1] ? A[1] - A[0] < Written : A[0] - A[1] < Length + 1)
      return failure(Complete ? diagnostic::TerminatedStringCopyOverlap
                              : diagnostic::SizedStringCopyOverlap);
    Text->resize(Copied);
    Text->push_back('\0');
    if (auto E = access(A[0], Written, Write))
      return std::move(E);
    if (auto E = CPU.write(
            A[0],
            llvm::ArrayRef(reinterpret_cast<const uint8_t *>(Text->data()),
                           Text->size())))
      return std::move(E);
    return value(Complete ? A[0] : Length);
  }
  if (!Count)
    return value(A[0]);
  if (auto E = access(A[0], Count, Write))
    return std::move(E);
  // Stage the entire effect. Read only through the first NUL (or Count),
  // while still writing exactly Count bytes, including zero padding.
  std::vector<uint8_t> Bytes(Count, 0);
  uint64_t ReadCount = 0;
  for (uint64_t I = 0; I < Count; ++I) {
    if (A[1] > UINT64_MAX - I)
      return failure(diagnostic::StringAddressOverflow);
    auto C = byte(A[1] + I);
    if (!C)
      return C.takeError();
    Bytes[I] = *C;
    ReadCount = I + 1;
    if (!*C)
      break;
  }
  // Overlap has no defined C result. Include padding in the destination
  // extent, but do not require Count readable bytes after a source NUL.
  if (A[0] <= A[1] ? A[1] - A[0] < Count : A[0] - A[1] < ReadCount)
    return failure(diagnostic::StringCopyOverlap);
  // Recheck the deadline after staging, before publishing any guest bytes.
  if (auto E = access(A[0], Count, Write))
    return std::move(E);
  // API 28 checks the consumed source extent after copying, including NUL.
  // Retain the shared model's atomic refusal instead of pre-abort writes.
  if (Name == symbol::StrncpyChk2 && ReadCount > A[4])
    return failure(diagnostic::FortifyReadPrefix + Name);
  if (auto E = CPU.write(A[0], Bytes))
    return std::move(E);
  return value(A[0]);
}

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

BionicResult Bionic::findSubstring(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  auto Needle = string(A[1]);
  if (!Needle)
    return Needle.takeError();
  if (Needle->empty())
    return value(A[0]);
  // A prefix table keeps repetitive inputs linear. Only the needed haystack
  // prefix is read: neither a match nor NUL permits a later guest read.
  if (Needle->size() > Options.MemoryLimit / sizeof(size_t))
    return failure(diagnostic::StringScanLimit);
  std::vector<size_t> Prefix(Needle->size());
  for (size_t I = 1, J = 0; I < Needle->size(); ++I) {
    if (!Budget.remainingMicroseconds()) {
      Expired = true;
      return failure(diagnostic::CallTimeout);
    }
    while (J && (*Needle)[I] != (*Needle)[J])
      J = Prefix[J - 1];
    if ((*Needle)[I] == (*Needle)[J])
      ++J;
    Prefix[I] = J;
  }
  for (uint64_t I = 0, J = 0; I < Options.MemoryLimit; ++I) {
    if (A[0] > UINT64_MAX - I)
      return failure(diagnostic::StringAddressOverflow);
    auto C = byte(A[0] + I);
    if (!C)
      return C.takeError();
    if (!*C)
      return value(0);
    while (J && *C != static_cast<uint8_t>((*Needle)[J]))
      J = Prefix[J - 1];
    if (*C == static_cast<uint8_t>((*Needle)[J]))
      ++J;
    if (J == Needle->size())
      return value(A[0] + I - (J - 1));
  }
  return failure(diagnostic::StringScanLimit);
}

BionicResult Bionic::spanString(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  const bool RejectMembers = Call.Name == symbol::Strcspn;
  std::array<bool, 256> Members{};
  bool Terminated = false;
  // Build a fixed-size byte set without copying guest strings to host libc.
  // Both scans use the same access/deadline checks as other string models.
  for (uint64_t I = 0; I < Options.MemoryLimit; ++I) {
    if (A[1] > UINT64_MAX - I)
      return failure(diagnostic::StringAddressOverflow);
    auto C = byte(A[1] + I);
    if (!C)
      return C.takeError();
    if (!*C) {
      Terminated = true;
      break;
    }
    Members[*C] = true;
  }
  if (!Terminated)
    return failure(diagnostic::StringScanLimit);
  for (uint64_t I = 0; I < Options.MemoryLimit; ++I) {
    if (A[0] > UINT64_MAX - I)
      return failure(diagnostic::StringAddressOverflow);
    auto C = byte(A[0] + I);
    if (!C)
      return C.takeError();
    // Both spans stop at NUL, including strcspn with an empty reject set.
    // No later source byte is needed once membership ends the span.
    if (!*C || Members[*C] == RejectMembers)
      return value(I);
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
    if (!*V) {
      // API 28 checks the object extent only after strlen has reached NUL.
      // The terminator must fit too; scan faults and budgets take precedence.
      if (Name == symbol::StrlenChk && I >= A[1])
        return failure(diagnostic::FortifyReadPrefix + Name);
      return value(I);
    }
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
