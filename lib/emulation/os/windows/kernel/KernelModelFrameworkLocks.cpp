//===- KernelModelFrameworkLocks.cpp - WDF executive/dispatcher bridge ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// Normal WDF spin locks use the executive lock authority. Wait locks use the
/// dispatcher, with actual thread ownership, APC state and virtual deadlines.
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
llvm::Error lockError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "framework lock: " + Message);
}
using LockOwner = KernelDispatcher::WaitLockOwner;
} // namespace

void KernelModel::configureFrameworkLockHost() {
  KernelFramework::LockHost Host;
  Host.Create = [this](bool Waitable) -> llvm::Expected<uint64_t> {
    auto Storage =
        allocate(Waitable ? dispatcher::EventSize : profile::PointerSize);
    if (!Storage)
      return Storage.takeError();
    if (Waitable) {
      if (auto E = Dispatcher.initializeWaitLock(*Storage))
        return E;
    } else {
      auto Initialized = initializeSpinLock(*Storage);
      if (!Initialized)
        return Initialized.takeError();
    }
    FrameworkLockStorage.emplace(*Storage, Waitable);
    return *Storage;
  };
  Host.CanDelete = [this](uint64_t Storage, bool Waitable) -> llvm::Error {
    auto Lock = FrameworkLockStorage.find(Storage);
    if (Lock == FrameworkLockStorage.end() || Lock->second != Waitable)
      return lockError("deletion requires matching live lock storage");
    return canReleaseRange(Storage, Waitable ? dispatcher::EventSize
                                             : profile::PointerSize);
  };
  Host.Destroy = [this](uint64_t Storage, bool Waitable) -> llvm::Error {
    auto Lock = FrameworkLockStorage.find(Storage);
    if (Lock == FrameworkLockStorage.end() || Lock->second != Waitable)
      return lockError("destruction requires matching live lock storage");
    if (auto E = prepareReleaseRange(Storage, Waitable ? dispatcher::EventSize
                                                       : profile::PointerSize))
      return E;
    FrameworkLockStorage.erase(Lock);
    return llvm::Error::success();
  };
  Host.Acquire =
      [this](uint64_t Storage, bool Waitable,
             std::optional<int64_t> Timeout) -> llvm::Expected<uint32_t> {
    auto Lock = FrameworkLockStorage.find(Storage);
    if (Lock == FrameworkLockStorage.end() || Lock->second != Waitable ||
        !CurrentExecution || !CurrentThreadKey)
      return lockError("acquisition requires a live lock and guest thread");
    if (!Waitable) {
      auto Acquired = acquireSpinLock(Storage, SpinLockMode::Raise);
      if (!Acquired)
        return Acquired.takeError();
      return windows::StatusSuccess;
    }
    if (PendingWait)
      return lockError("acquisition cannot replace a pending wait");
    Wait Pending;
    Pending.Type = Wait::Kind::FrameworkWaitLock;
    Pending.Object = Storage;
    Pending.Execution = CurrentExecution;
    Pending.Thread = CurrentThreadKey;
    Pending.IRQL = CurrentIRQL;
    if (Timeout) {
      auto Deadline = Scheduler.computeDeadline(*Timeout);
      if (!Deadline)
        return Deadline.takeError();
      Pending.Deadline = *Deadline;
    }
    auto &Apc = ApcStates[CurrentThreadKey];
    if (Apc.CriticalDepth == profile::MaxAPCRegionNesting)
      return lockError("critical-region nesting limit exceeded");
    auto Acquired = Dispatcher.tryAcquireWaitLock(
        Storage, {LockOwner::Kind::Thread, CurrentThreadKey});
    if (!Acquired)
      return Acquired.takeError();
    if (*Acquired) {
      ++Apc.CriticalDepth;
      FrameworkWaitLockThreads.emplace(
          std::make_pair(Storage, CurrentThreadKey), CurrentExecution);
      return windows::StatusSuccess;
    }
    if (Pending.Deadline && *Pending.Deadline <= Scheduler.now100ns())
      return windows::StatusTimeout;
    ++Apc.CriticalDepth;
    ++WaitReferences[Storage];
    PendingWait = Pending;
    return windows::StatusSuccess;
  };
  Host.Release = [this](uint64_t Storage, bool Waitable) -> llvm::Error {
    auto Lock = FrameworkLockStorage.find(Storage);
    if (Lock == FrameworkLockStorage.end() || Lock->second != Waitable)
      return lockError("release requires matching live lock storage");
    if (!Waitable) {
      auto Held = ExecutiveSpinLocks.find(Storage);
      if (Held == ExecutiveSpinLocks.end())
        return lockError("spin lock has not been acquired");
      auto Released = releaseSpinLock(Storage, Held->second.OldIRQL);
      if (!Released)
        return Released.takeError();
      return llvm::Error::success();
    }
    auto Held = FrameworkWaitLockThreads.find({Storage, CurrentThreadKey});
    auto Apc = ApcStates.find(CurrentThreadKey);
    if (Held == FrameworkWaitLockThreads.end() || Apc == ApcStates.end() ||
        !Apc->second.CriticalDepth)
      return lockError(
          "wait lock release requires its acquiring thread and APC region");
    if (auto E = Dispatcher.releaseWaitLock(
            Storage, {LockOwner::Kind::Thread, CurrentThreadKey}))
      return E;
    --Apc->second.CriticalDepth;
    FrameworkWaitLockThreads.erase(Held);
    return llvm::Error::success();
  };
  Framework->setLockHost(std::move(Host));
}

llvm::Expected<std::optional<uint32_t>>
KernelModel::pollFrameworkWaitLock(const Wait &Pending) {
  auto Acquired = Dispatcher.tryAcquireWaitLock(
      Pending.Object, {LockOwner::Kind::Thread, Pending.Thread});
  if (!Acquired)
    return Acquired.takeError();
  const bool Expired =
      Pending.Deadline && *Pending.Deadline <= Scheduler.now100ns();
  if (!*Acquired && !Expired)
    return std::optional<uint32_t>{};
  auto Reference = WaitReferences.find(Pending.Object);
  auto Apc = ApcStates.find(Pending.Thread);
  if (Reference == WaitReferences.end() || !Reference->second ||
      Apc == ApcStates.end() || !Apc->second.CriticalDepth)
    return lockError("wait completion lost its retained lock or APC region");
  if (!--Reference->second)
    WaitReferences.erase(Reference);
  if (*Acquired)
    FrameworkWaitLockThreads.emplace(
        std::make_pair(Pending.Object, Pending.Thread), Pending.Execution);
  else
    --Apc->second.CriticalDepth;
  return std::optional<uint32_t>{*Acquired ? windows::StatusSuccess
                                           : windows::StatusTimeout};
}
} // namespace neverd::emulation
