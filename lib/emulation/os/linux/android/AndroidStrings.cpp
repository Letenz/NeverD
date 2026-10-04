//===- AndroidStrings.cpp - Bounded guest string transformations ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

#include <array>

namespace neverd::emulation::android_model {
llvm::Expected<uint64_t> Bionic::tokenize(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  uint64_t Input = A[0];
  if (A[2] % 8)
    return failure("unaligned strtok_r save pointer");
  if (!Input) {
    if (auto E = access(A[2], 8, Read))
      return std::move(E);
    uint8_t Saved[8];
    if (auto E = CPU.read(A[2], Saved))
      return std::move(E);
    Input = llvm::support::endian::read64le(Saved);
    // API 28 leaves an exhausted context untouched, without reading delim.
    if (!Input)
      return uint64_t(0);
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
      return failure("strtok_r string address overflows");
    uint64_t Address = Input + I;
    auto C = byte(Address);
    if (!C)
      return C.takeError();
    bool Split = Token && *C && Separators[*C];
    if (!*C || Split) {
      if (Split && Address == UINT64_MAX)
        return failure("strtok_r continuation address overflows");
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
      return Token.value_or(0);
    }
    if (!Separators[*C] && !Token)
      Token = Address;
  }
  return failure("strtok_r scan exceeds memory limit");
}
} // namespace neverd::emulation::android_model
