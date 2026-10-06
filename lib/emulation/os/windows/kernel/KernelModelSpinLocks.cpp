//===- KernelModelSpinLocks.cpp - Executive spin locks --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded single-processor executive spin locks and IRQL restoration.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"

#include "neverd/emulation/DriverProfile.h"

namespace neverd::emulation {
namespace {
llvm::Error spinLockError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
} // namespace

llvm::Error KernelModel::validateSpinLockAddress(uint64_t Address) const {
  if (!Address || (Address & 7))
    return spinLockError("executive spin lock requires aligned kernel storage");
  if (Interrupts.usesSpinLock(Address))
    return spinLockError(
        "connected interrupt spin lock requires its interrupt APIs");
  return llvm::Error::success();
}

llvm::Error KernelModel::validateSpinLockStorage(uint64_t Address) const {
  if (auto E = validateGuestAccess(Address, 8, true))
    return E;
  auto Writable = Memory.canAccess(Address, 8, Read | Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return spinLockError("executive spin lock requires writable storage");
  for (const auto &[Pool, Allocation] : Allocations)
    if (Address >= Pool && Address - Pool < Allocation.Size &&
        (!Allocation.NonPaged || 8 > Allocation.Size - (Address - Pool)))
      return spinLockError("executive spin lock requires nonpaged storage");

  return llvm::Error::success();
}

llvm::Expected<uint64_t> KernelModel::initializeSpinLock(uint64_t Address) {
  if (auto E = validateSpinLockAddress(Address))
    return E;
  if (Address < profile::UserProbeLimit)
    return spinLockError("executive spin lock requires kernel storage");
  if (ExecutiveSpinLocks.count(Address))
    return spinLockError("cannot initialize a held executive spin lock");
  if (auto E = validateSpinLockStorage(Address))
    return E;
  if (auto E = Memory.writeInteger(Address, 0, 8))
    return E;
  return 0;
}

llvm::Expected<uint64_t> KernelModel::acquireSpinLock(uint64_t Address,
                                                      SpinLockMode Mode) {
  if (auto E = validateSpinLockAddress(Address))
    return E;
  if (Address < profile::UserProbeLimit)
    return spinLockError("executive spin lock requires kernel storage");
  const bool AtDpc = Mode != SpinLockMode::Raise;
  if (AtDpc && CurrentIRQL != scheduler::DispatchLevel)
    return spinLockError("DPC-level spin-lock acquisition requires "
                         "DISPATCH_LEVEL");
  if (!CurrentExecution)
    return spinLockError("executive spin lock requires an active guest thread");
  if (ExecutiveSpinLocks.count(Address)) {
    if (Mode == SpinLockMode::TryAtDpc)
      return 0;
    return spinLockError("executive spin lock would deadlock on this "
                         "cooperative processor");
  }
  if (auto E = validateSpinLockStorage(Address))
    return E;
  auto Value = Memory.readInteger(Address, 8);
  if (!Value)
    return Value.takeError();
  if (*Value)
    return spinLockError("executive spin lock must be initialized and free");
  if (auto E = Memory.writeInteger(Address, 1, 8))
    return E;
  const uint8_t OldIRQL = CurrentIRQL;
  CurrentIRQL = scheduler::DispatchLevel;
  ExecutiveSpinLocks.emplace(
      Address, ExecutiveSpinLock{CurrentExecution, OldIRQL, !AtDpc});
  if (Mode == SpinLockMode::Raise)
    return OldIRQL;
  return Mode == SpinLockMode::TryAtDpc ? 1 : 0;
}

llvm::Expected<uint64_t>
KernelModel::releaseSpinLock(uint64_t Address,
                             std::optional<uint8_t> RestoreIRQL) {
  if (auto E = validateSpinLockAddress(Address))
    return E;
  auto Lock = ExecutiveSpinLocks.find(Address);
  if (Lock == ExecutiveSpinLocks.end() ||
      Lock->second.Execution != CurrentExecution)
    return spinLockError("executive spin lock is not owned by this thread");
  if (CurrentIRQL != scheduler::DispatchLevel)
    return spinLockError("executive spin lock release requires DISPATCH_LEVEL");
  if (Lock->second.RaisedIRQL != RestoreIRQL.has_value())
    return spinLockError("executive spin lock release variant does not "
                         "match acquisition");
  if (RestoreIRQL && *RestoreIRQL != Lock->second.OldIRQL)
    return spinLockError("executive spin lock release must restore the "
                         "saved IRQL");
  auto Value = Memory.readInteger(Address, 8);
  if (!Value)
    return Value.takeError();
  if (*Value != 1)
    return spinLockError("executive spin lock storage was modified");
  if (auto E = Memory.writeInteger(Address, 0, 8))
    return E;
  CurrentIRQL = RestoreIRQL ? Lock->second.OldIRQL : scheduler::DispatchLevel;
  ExecutiveSpinLocks.erase(Lock);
  return 0;
}
} // namespace neverd::emulation
