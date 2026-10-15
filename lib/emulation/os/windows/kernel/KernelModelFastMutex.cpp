//===- KernelModelFastMutex.cpp - Fast/guarded mutex ownership ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Uncontended Windows 8+ mutex acquisition and nonblocking try semantics.
/// Contended waits require the kernel AutoBoost protocol and stop explicitly.
/// The dispatcher owns embedded events; this model owns thread and IRQL state.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
using namespace windows;
llvm::Error fastMutexError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
} // namespace

bool KernelModel::threadHoldsFastMutex(uint64_t Thread) const {
  return std::any_of(
      FastMutexHolds.begin(), FastMutexHolds.end(),
      [=](const auto &Entry) { return Entry.second.Thread == Thread; });
}

llvm::Error KernelModel::validateFastMutexAccess(uint64_t Address,
                                                 uint32_t Size,
                                                 bool IsWrite) const {
  if (IsWrite)
    for (const auto &[Base, Hold] : FastMutexHolds)
      if (Size && Address < Base + FastMutexSize && Base < Address + Size)
        return fastMutexError("guest write to a held fast mutex");
  return llvm::Error::success();
}

llvm::Error KernelModel::validateFastMutexContext(uint8_t IRQL,
                                                  uint16_t Critical,
                                                  uint16_t Guarded,
                                                  uint64_t Retiring) const {
  for (const auto &[Address, Hold] : FastMutexHolds) {
    if (Hold.Thread != CurrentThreadKey || Address == Retiring)
      continue;
    if (IRQL >= APCLevel)
      continue;
    if (Hold.Mode == FastMutexMode::Normal)
      return fastMutexError("cannot lower IRQL while holding a fast mutex");
    if (!Guarded && (Hold.Mode == FastMutexMode::UnsafeGuarded || !Critical))
      return fastMutexError(
          "unsafe fast mutex requires its APC-disable region or APC_LEVEL");
  }
  return llvm::Error::success();
}

llvm::Error KernelModel::validateFastMutexStorage(uint64_t Address,
                                                  bool Initialize) const {
  if (Address < profile::UserProbeLimit || (Address & 7) ||
      FastMutexSize > UINT64_MAX - Address)
    return fastMutexError(
        "fast mutex requires aligned resident kernel storage");
  // Only the embedded event may overlap dispatcher storage. Validate the
  // surrounding fields normally, then all other ownership across the object.
  // A known hold authorizes its own release stores. New ownership must first
  // establish write authority over every surrounding field.
  const bool NewOwnership = !FastMutexHolds.count(Address);
  if (auto E = validateGuestAccess(Address, FastMutexEventOffset, NewOwnership))
    return E;
  if (auto E = validateGuestAccess(Address + FastMutexOldIrqlOffset,
                                   FastMutexSize - FastMutexOldIrqlOffset,
                                   NewOwnership))
    return E;
  if (auto E = validateGuestAccessImpl(Address, FastMutexSize, false, false))
    return E;
  for (const auto &[Base, Allocation] : Allocations)
    if (!Allocation.NonPaged && Address < Base + Allocation.Size &&
        Base < Address + FastMutexSize)
      return fastMutexError("fast mutex storage must be nonpaged");
  if (auto E = validateUnborrowedKernelStorage(Address, FastMutexSize))
    return E;
  auto Writable = Memory.canAccess(Address, FastMutexSize, Read | Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return fastMutexError("fast mutex operation requires writable storage");
  if (auto E = Memory.validateBacking(Address, FastMutexSize))
    return E;
  if (!Initialize)
    return Dispatcher.validateFastMutexEvent(Address + FastMutexEventOffset);
  return llvm::Error::success();
}

llvm::Expected<uint64_t> KernelModel::initializeFastMutex(uint64_t Address) {
  // KeInitializeGuardedMutex, RVA 0x3609e0: Count=1, Owner/Contention=0,
  // an empty synchronization event, with OldIrql and padding untouched.
  if (auto E = validateFastMutexStorage(Address, true))
    return E;
  if (auto E = canRevokeVirtualRange(Address, FastMutexSize))
    return E;
  if (auto E =
          Dispatcher.initializeFastMutexEvent(Address + FastMutexEventOffset))
    return E;
  if (auto E = Memory.writeInteger(Address, 1, 4))
    return E;
  if (auto E = Memory.writeInteger(Address + FastMutexOwnerOffset, 0, 8))
    return E;
  if (auto E = Memory.writeInteger(Address + FastMutexContentionOffset, 0, 4))
    return E;
  return 0;
}

llvm::Expected<uint64_t>
KernelModel::acquireFastMutex(uint64_t Address, FastMutexMode Mode, bool Try) {
  if (!CurrentExecution || !CurrentThreadKey)
    return fastMutexError(
        "fast mutex acquisition requires an active guest thread");
  const auto State = ApcStates.find(CurrentThreadKey);
  const uint16_t Critical =
      State == ApcStates.end() ? 0 : State->second.CriticalDepth;
  const uint16_t Guarded =
      State == ApcStates.end() ? 0 : State->second.GuardedDepth;
  if (Mode != FastMutexMode::Normal && CurrentIRQL < APCLevel && !Guarded &&
      (Mode == FastMutexMode::UnsafeGuarded || !Critical))
    return fastMutexError("unsafe fast mutex acquisition requires an "
                          "APC-disable region or APC_LEVEL");
  if (auto E = validateFastMutexStorage(Address, false))
    return E;
  auto Count = Memory.readInteger(Address, 4);
  if (!Count)
    return Count.takeError();
  // Normal and try acquisition clear Count bit 0; only the free and owned
  // states are supported. Waiter counts belong to the unmodeled slow path.
  if (*Count != 0 && *Count != 1)
    return fastMutexError("fast mutex has an unsupported waiter count");
  if (!*Count) {
    if (Try)
      return 0;
    return fastMutexError("contended or recursive fast mutex acquisition "
                          "requires unsupported AutoBoost waiting");
  }
  if (FastMutexHolds.count(Address))
    return fastMutexError("held fast mutex count was modified");
  auto Owner = Memory.readInteger(Address + FastMutexOwnerOffset, 8);
  if (!Owner)
    return Owner.takeError();
  if (*Owner)
    return fastMutexError("free fast mutex has a non-null owner");
  if (auto E = canRevokeVirtualRange(Address, FastMutexSize))
    return E;
  auto Object = currentThreadObject();
  if (!Object)
    return Object.takeError();
  const uint8_t OldIRQL = CurrentIRQL;
  if (auto E = Memory.writeInteger(Address, 0, 4))
    return E;
  if (auto E = Memory.writeInteger(Address + FastMutexOwnerOffset, *Object, 8))
    return E;
  // All normal target exports write a full ULONG at +0x30, not just a KIRQL.
  // Unsafe variants leave both this field and the current IRQL unchanged.
  if (Mode == FastMutexMode::Normal) {
    if (auto E =
            Memory.writeInteger(Address + FastMutexOldIrqlOffset, OldIRQL, 4))
      return E;
    CurrentIRQL = APCLevel;
  }
  FastMutexHolds.emplace(
      Address, FastMutexHold{CurrentThreadKey, *Object, OldIRQL, Mode});
  return Try ? 1 : 0;
}

llvm::Expected<uint64_t> KernelModel::releaseFastMutex(uint64_t Address,
                                                       bool Unsafe) {
  auto It = FastMutexHolds.find(Address);
  if (It == FastMutexHolds.end() || It->second.Thread != CurrentThreadKey)
    return fastMutexError("fast mutex release requires the owning thread");
  const FastMutexHold Hold = It->second;
  if ((Hold.Mode != FastMutexMode::Normal) != Unsafe)
    return fastMutexError(
        "fast mutex release variant does not match acquisition");
  if (!Unsafe && CurrentIRQL != APCLevel)
    return fastMutexError("fast mutex release requires APC_LEVEL");
  if (auto E = validateFastMutexStorage(Address, false))
    return E;
  auto Count = Memory.readInteger(Address, 4);
  if (!Count)
    return Count.takeError();
  auto Owner = Memory.readInteger(Address + FastMutexOwnerOffset, 8);
  if (!Owner)
    return Owner.takeError();
  if (*Count || *Owner != Hold.Object)
    return fastMutexError("held fast mutex count or owner was modified");
  if (!Unsafe) {
    auto Saved = Memory.readInteger(Address + FastMutexOldIrqlOffset, 4);
    if (!Saved)
      return Saved.takeError();
    if (*Saved != Hold.OldIRQL)
      return fastMutexError("fast mutex saved IRQL was modified");
  }
  const auto State = ApcStates.find(CurrentThreadKey);
  const uint16_t Critical =
      State == ApcStates.end() ? 0 : State->second.CriticalDepth;
  const uint16_t Guarded =
      State == ApcStates.end() ? 0 : State->second.GuardedDepth;
  if (auto E = validateFastMutexContext(CurrentIRQL, Critical, Guarded))
    return E;
  const uint8_t Restored = Unsafe ? CurrentIRQL : Hold.OldIRQL;
  if (auto E = validateFastMutexContext(Restored, Critical, Guarded, Address))
    return E;
  if (auto E = validateExecutiveResourceRestoreIRQL(Restored))
    return E;
  // ExReleaseFastMutex and KeReleaseGuardedMutex share RVA 0x2c9310.
  // Unsafe releases use 0x206930, directly or through its guarded wrapper.
  if (auto E = Memory.writeInteger(Address + FastMutexOwnerOffset, 0, 8))
    return E;
  if (auto E = Memory.writeInteger(Address, 1, 4))
    return E;
  FastMutexHolds.erase(It);
  CurrentIRQL = Restored;
  return 0;
}
} // namespace neverd::emulation
