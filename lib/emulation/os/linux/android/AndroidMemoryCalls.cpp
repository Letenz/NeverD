//===- AndroidMemoryCalls.cpp - Bounded Bionic memory operations ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"

#include "neverd/emulation/CPU.h"

namespace neverd::emulation::android_model {
llvm::Error Bionic::access(uint64_t Address, uint64_t Size,
                           unsigned Permissions) {
  if (!Budget.remainingMicroseconds()) {
    Expired = true;
    return failure(diagnostic::CallTimeout);
  }
  if (!Size)
    return llvm::Error::success();
  if (Size > Options.MemoryLimit)
    return failure(diagnostic::MemoryLimit);
  auto Allowed = CPU.canAccess(Address, Size, Permissions | UserAccessible);
  if (!Allowed)
    return Allowed.takeError();
  if (!*Allowed)
    return failure(diagnostic::GuestPointer);
  return llvm::Error::success();
}
llvm::Expected<uint8_t> Bionic::byte(uint64_t Address) {
  if (auto E = access(Address, 1, Read))
    return std::move(E);
  uint8_t Value;
  if (auto E = CPU.read(Address, llvm::MutableArrayRef<uint8_t>(&Value, 1)))
    return std::move(E);
  return Value;
}
BionicResult Bionic::copyMemory(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  uint64_t Size = A[2];
  if (auto E = access(A[0], Size, Write))
    return std::move(E);
  if (auto E = access(A[1], Size, Read))
    return std::move(E);
  if (Call.Name == symbol::Memcpy && Size &&
      (A[0] <= A[1] ? A[1] - A[0] < Size : A[0] - A[1] < Size))
    return failure(diagnostic::MemcpyOverlap);
  std::vector<uint8_t> Bytes(Size);
  if (Size) {
    if (auto E = CPU.read(A[1], Bytes))
      return std::move(E);
    if (auto E = CPU.write(A[0], Bytes))
      return std::move(E);
  }
  return value(A[0]);
}

BionicResult Bionic::setMemory(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  uint64_t Size = A[2];
  if (auto E = access(A[0], Size, Write))
    return std::move(E);
  std::vector<uint8_t> Bytes(Size, static_cast<uint8_t>(A[1]));
  if (Size)
    if (auto E = CPU.write(A[0], Bytes))
      return std::move(E);
  return value(A[0]);
}

BionicResult Bionic::compareMemory(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  uint64_t Size = A[2];
  if (auto E = access(A[0], Size, Read))
    return std::move(E);
  if (auto E = access(A[1], Size, Read))
    return std::move(E);
  std::vector<uint8_t> Right(Size);
  // Preserve the admitted read order, including overlapping input ranges.
  if (Size)
    if (auto E = CPU.read(A[1], Right))
      return std::move(E);
  std::vector<uint8_t> Left(Size);
  if (Size)
    if (auto E = CPU.read(A[0], Left))
      return std::move(E);
  for (uint64_t I = 0; I < Size; ++I)
    if (Left[I] != Right[I])
      return value(static_cast<uint32_t>(int(Left[I]) - int(Right[I])));
  return value(0);
}

} // namespace neverd::emulation::android_model
