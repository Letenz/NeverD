//===- AndroidAllocation.cpp - Bionic heap ownership ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"

#include "neverd/emulation/CPU.h"

#include <algorithm>

namespace neverd::emulation::android_model {
llvm::Expected<uint64_t> Bionic::allocate(uint64_t Size) {
  uint64_t Effective = std::max(uint64_t(1), Size);
  if (Effective > Options.MemoryLimit ||
      Effective > UINT64_MAX - PageSize + 1) {
    if (auto E = setErrno(linux_model::NoMemory))
      return std::move(E);
    return uint64_t(0);
  }
  uint64_t Mapped = (Effective + PageSize - 1) & ~(PageSize - 1);
  ProcessServiceEvent Event{
      0,
      0,
      {0, Mapped, linux_model::ProtRead | linux_model::ProtWrite,
       linux_model::MapPrivate | linux_model::MapAnonymous, UINT64_MAX, 0},
      std::nullopt};
  auto Address = Kernel.handle(linux_model::ServiceKind::Mmap, Event);
  if (!Address)
    return Address.takeError();
  if (!*Address)
    return failure(diagnostic::AllocatorReturn);
  if (**Address >= uint64_t(0) - 4095) {
    if (auto E = setErrno(uint64_t(0) - **Address))
      return std::move(E);
    return uint64_t(0);
  }
  Allocations.emplace(**Address, Allocation{Size, Mapped});
  return **Address;
}
llvm::Error Bionic::release(uint64_t Address) {
  if (!Address)
    return llvm::Error::success();
  auto I = Allocations.find(Address);
  if (I == Allocations.end())
    return failure(diagnostic::AllocationOwnership);
  ProcessServiceEvent Event{
      0, 0, {Address, I->second.MappedSize, 0, 0, 0, 0}, std::nullopt};
  auto Returned = Kernel.handle(linux_model::ServiceKind::Munmap, Event);
  if (!Returned)
    return Returned.takeError();
  if (!*Returned || **Returned)
    return failure(diagnostic::AllocationRelease);
  Allocations.erase(I);
  return llvm::Error::success();
}
BionicResult Bionic::malloc(const NativeCallEvent &Call) {
  auto Address = allocate(Call.Arguments[0]);
  if (!Address)
    return Address.takeError();
  return value(*Address);
}

BionicResult Bionic::calloc(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  if (A[0] && A[1] > UINT64_MAX / A[0]) {
    if (auto E = setErrno(linux_model::NoMemory))
      return std::move(E);
    return value(0);
  }
  // Anonymous allocations start zeroed.
  auto Address = allocate(A[0] * A[1]);
  if (!Address)
    return Address.takeError();
  return value(*Address);
}

BionicResult Bionic::realloc(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  uint64_t Size = A[1];
  std::vector<uint8_t> Saved;
  if (A[0]) {
    auto I = Allocations.find(A[0]);
    if (I == Allocations.end())
      return failure(diagnostic::ReallocationOwnership);
    uint64_t Copy = std::min(Size, I->second.Size);
    if (auto E = access(A[0], Copy, Read))
      return std::move(E);
    Saved.resize(Copy);
    if (Copy)
      if (auto E = CPU.read(A[0], Saved))
        return std::move(E);
  }
  auto Address = allocate(Size);
  if (!Address)
    return Address.takeError();
  if (!*Address)
    return value(0);
  // Anonymous allocations keep the growth region zeroed.
  if (!Saved.empty())
    if (auto E = CPU.write(*Address, Saved))
      return std::move(E);
  if (auto E = release(A[0]))
    return std::move(E);
  return value(*Address);
}

BionicResult Bionic::free(const NativeCallEvent &Call) {
  if (auto E = release(Call.Arguments[0]))
    return std::move(E);
  return value(0); // The ABI leaves x0 unspecified for void calls.
}

} // namespace neverd::emulation::android_model
