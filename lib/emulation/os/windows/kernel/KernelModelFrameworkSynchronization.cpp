//===- KernelModelFrameworkSynchronization.cpp - Callback locks ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Execute framework callback locks on the shared dispatcher authority. A
/// waiting callback retains its real frame, object and thread identity.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
llvm::Error synchronizationError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "framework synchronization: " + Message);
}
} // namespace

llvm::Expected<uint64_t> KernelModel::frameworkCallbackLock(uint64_t Object) {
  auto Existing = FrameworkCallbackLocks.find(Object);
  if (Existing != FrameworkCallbackLocks.end())
    return Existing->second.Storage;
  auto Storage = allocate(dispatcher::EventSize);
  if (!Storage)
    return Storage.takeError();
  if (auto E = Dispatcher.initializeWaitLock(*Storage, true))
    return E;
  FrameworkCallbackLocks.emplace(Object, FrameworkCallbackLock{*Storage});
  return *Storage;
}

llvm::Expected<bool> KernelModel::acquireFrameworkCallback(uint64_t Object,
                                                           bool Automatic) {
  if (!Object)
    return true;
  if (!Framework || !CurrentExecution || !CurrentThreadKey || PendingWait)
    return synchronizationError("entry requires a live framework thread");
  const uint8_t LockIRQL = Framework->synchronizationIRQL(Object);
  const bool Passive = LockIRQL == scheduler::PassiveLevel;
  if (CurrentIRQL > (Passive ? windows::APCLevel : scheduler::DispatchLevel))
    return synchronizationError("entry exceeds the callback lock IRQL");
  const auto Key = std::make_pair(CurrentExecution, Object);
  auto Entry = FrameworkCallbackEntries.find(Key);
  if (Entry == FrameworkCallbackEntries.end()) {
    if (!Automatic &&
        std::any_of(FrameworkCallbackEntries.begin(),
                    FrameworkCallbackEntries.end(), [&](const auto &I) {
                      return I.second.Object == Object &&
                             I.second.Thread == CurrentThreadKey;
                    }))
      return synchronizationError("manual callback locking is not recursive");
    auto &APC = ApcStates[CurrentThreadKey];
    if (Passive && APC.CriticalDepth == profile::MaxAPCRegionNesting)
      return synchronizationError("callback critical-region limit exceeded");
    auto Storage = frameworkCallbackLock(Object);
    if (!Storage)
      return Storage.takeError();
    if (auto E = Framework->retainSynchronizationObject(Object))
      return E;
    FrameworkCallbackEntry State;
    State.Object = Object;
    State.Thread = CurrentThreadKey;
    State.PreviousIRQL = CurrentIRQL;
    State.Passive = Passive;
    State.Automatic = Automatic;
    State.CriticalDepth = APC.CriticalDepth;
    if (Passive)
      ++APC.CriticalDepth;
    Entry = FrameworkCallbackEntries.emplace(Key, State).first;
  }
  auto &State = Entry->second;
  if (State.Acquired || State.Automatic != Automatic ||
      State.Thread != CurrentThreadKey)
    return synchronizationError("callback entry has inconsistent ownership");
  const uint64_t Storage = FrameworkCallbackLocks.at(Object).Storage;
  auto Acquired = Dispatcher.tryAcquireWaitLock(
      Storage, {KernelDispatcher::WaitLockOwner::Kind::Thread, State.Thread});
  if (!Acquired)
    return Acquired.takeError();
  if (*Acquired) {
    State.Acquired = true;
    if (!Passive)
      CurrentIRQL = scheduler::DispatchLevel;
    return true;
  }
  if (!Passive)
    return synchronizationError("contended callback spin lock cannot block");
  Wait Pending;
  Pending.Type = Automatic ? Wait::Kind::FrameworkCallback
                           : Wait::Kind::FrameworkCallbackLock;
  Pending.Object = Storage;
  Pending.Execution = CurrentExecution;
  Pending.Thread = CurrentThreadKey;
  Pending.IRQL = CurrentIRQL;
  ++WaitReferences[Storage];
  PendingWait = Pending;
  return false;
}

llvm::Expected<bool> KernelModel::beginFrameworkCallback(uint64_t Object) {
  return acquireFrameworkCallback(Object, true);
}

llvm::Expected<std::optional<uint32_t>>
KernelModel::pollFrameworkCallback(const Wait &Pending) {
  auto Entry = std::find_if(
      FrameworkCallbackEntries.begin(), FrameworkCallbackEntries.end(),
      [&](const auto &I) {
        return I.first.first == Pending.Execution && !I.second.Acquired &&
               FrameworkCallbackLocks.at(I.second.Object).Storage ==
                   Pending.Object;
      });
  auto Reference = WaitReferences.find(Pending.Object);
  if (Entry == FrameworkCallbackEntries.end() ||
      Reference == WaitReferences.end() || !Reference->second)
    return synchronizationError("wait lost its callback lock ownership");
  const KernelDispatcher::WaitLockOwner Owner{
      KernelDispatcher::WaitLockOwner::Kind::Thread, Entry->second.Thread};
  if (!Dispatcher.waitLockAvailable(Pending.Object, Owner))
    return std::optional<uint32_t>{};
  if (!Entry->second.Automatic) {
    auto Acquired = Dispatcher.tryAcquireWaitLock(Pending.Object, Owner);
    if (!Acquired)
      return Acquired.takeError();
    if (!*Acquired)
      return synchronizationError("ready callback lock changed ownership");
    Entry->second.Acquired = true;
  }
  --Reference->second;
  return std::optional<uint32_t>{windows::StatusSuccess};
}

llvm::Error KernelModel::releaseFrameworkCallback(uint64_t Object,
                                                  bool Automatic) {
  if (!Object)
    return llvm::Error::success();
  auto Entry = FrameworkCallbackEntries.find({CurrentExecution, Object});
  if (Entry == FrameworkCallbackEntries.end() || !Entry->second.Acquired ||
      Entry->second.Thread != CurrentThreadKey ||
      Entry->second.Automatic != Automatic)
    return synchronizationError("release requires the matching callback owner");
  const auto State = Entry->second;
  if (CurrentIRQL !=
      (State.Passive ? State.PreviousIRQL : scheduler::DispatchLevel))
    return synchronizationError("callback changed its protected IRQL");
  auto &APC = ApcStates[State.Thread];
  if (State.Passive &&
      (!APC.CriticalDepth ||
       (Automatic && APC.CriticalDepth != State.CriticalDepth + 1)))
    return synchronizationError("callback changed its protected APC state");
  if (auto E = Dispatcher.releaseWaitLock(
          FrameworkCallbackLocks.at(Object).Storage,
          {KernelDispatcher::WaitLockOwner::Kind::Thread, State.Thread}))
    return E;
  if (State.Passive)
    --APC.CriticalDepth;
  CurrentIRQL = State.PreviousIRQL;
  FrameworkCallbackEntries.erase(Entry);
  return Framework->releaseSynchronizationObject(Object);
}

llvm::Error KernelModel::finishFrameworkCallback(uint64_t Object) {
  return releaseFrameworkCallback(Object, true);
}

llvm::Error KernelModel::flushFrameworkCallbackDestructions() {
  return Framework ? Framework->flushSynchronizationDestructions()
                   : llvm::Error::success();
}
} // namespace neverd::emulation
