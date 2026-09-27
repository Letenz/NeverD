//===- KernelModelDriverIRP.cpp - Caller-allocated IRP storage ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Own raw IRP allocations independently of dispatch and completion records.
/// Allocation creates no file, device route or result row. IoFreeIrp releases
/// only packet storage; attached MDLs retain their separate caller ownership.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <array>

namespace neverd::emulation {
namespace {
using namespace windows;

llvm::Error driverIRPError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "caller IRP: " + Message);
}
} // namespace

llvm::Error KernelModel::initializeIRPHeader(uint64_t IRP, uint8_t StackCount,
                                             uint8_t CurrentLocation) {
  if (!StackCount || StackCount > MaxIRPStackCount || !CurrentLocation ||
      CurrentLocation > uint32_t(StackCount) + 1)
    return driverIRPError("header requires a bounded stack and valid cursor");
  struct Field {
    uint64_t Offset;
    uint64_t Value;
    unsigned Width;
  };
  const uint64_t Size = IRPSize + uint64_t(StackCount) * StackSize;
  const uint64_t Stack =
      IRP + IRPSize + uint64_t(CurrentLocation - 1) * StackSize;
  for (const Field &F : std::array<Field, 5>{
           {{IRPTypeOffset, IRPType, sizeof(uint16_t)},
            {ObjectSizeOffset, Size, sizeof(uint16_t)},
            {IRPStackCountOffset, StackCount, sizeof(uint8_t)},
            {IRPLocationOffset, CurrentLocation, sizeof(uint8_t)},
            {IRPStackPointerOffset, Stack, sizeof(uint64_t)}}})
    if (auto E = Memory.writeInteger(IRP + F.Offset, F.Value, F.Width))
      return E;
  return llvm::Error::success();
}

llvm::Expected<uint64_t>
KernelModel::allocateDriverIRP(llvm::ArrayRef<uint64_t> Arguments) {
  const uint8_t Count = uint8_t(Arguments[0]);
  if (!Count || Count > MaxIRPStackCount)
    return driverIRPError("IoAllocateIrp requires a positive bounded CCHAR "
                          "stack count");
  if (uint8_t(Arguments[1]))
    return driverIRPError("IoAllocateIrp requires ChargeQuota to be FALSE");
  const uint64_t Size = IRPSize + uint64_t(Count) * StackSize;
  const uint64_t Start =
      (NextAllocation + PoolAlignment - 1) & ~(PoolAlignment - 1);
  if (Start > AllocationEnd || Size > AllocationEnd - Start)
    return 0;
  auto IRP = allocate(Size, PoolAlignment);
  if (!IRP)
    return IRP.takeError();
  if (auto E = initializeIRPHeader(*IRP, Count, Count + 1)) {
    FreedRanges.emplace(*IRP, Size);
    return E;
  }
  DriverIRPAllocation Allocation;
  Allocation.Size = Size;
  Allocation.StackCount = Count;
  DriverIRPs.emplace(*IRP, Allocation);
  return *IRP;
}

llvm::Error KernelModel::releaseDriverIRPStorage(uint64_t IRP) {
  auto Allocation = DriverIRPs.find(IRP);
  if (Allocation == DriverIRPs.end() || Allocation->second.StorageReleased)
    return driverIRPError("release requires live caller-allocated storage");
  if (auto E = prepareReleaseRange(IRP, Allocation->second.Size))
    return E;
  FreedRanges.emplace(IRP, Allocation->second.Size);
  Allocation->second.StorageReleased = true;
  return llvm::Error::success();
}

llvm::Error KernelModel::freeDriverIRP(uint64_t IRP) {
  const auto Allocation = DriverIRPs.find(IRP);
  if (Allocation == DriverIRPs.end())
    return driverIRPError("IoFreeIrp requires the exact base of a live "
                          "caller-allocated IRP");
  if (Allocation->second.StorageReleased)
    return driverIRPError("IoFreeIrp cannot release the same packet twice");
  if (Allocation->second.Submitted)
    return freeSubmittedDriverIRP(IRP);
  if (auto E = releaseDriverIRPStorage(IRP))
    return E;
  DriverIRPs.erase(Allocation);
  return llvm::Error::success();
}

} // namespace neverd::emulation
