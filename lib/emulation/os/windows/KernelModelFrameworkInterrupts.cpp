//===- KernelModelFrameworkInterrupts.cpp - WDF interrupt execution -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bridge framework identities to assigned interrupt connections and the
/// common scheduler. Resource, lock and callback ownership stay in WDM.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
llvm::Error frameworkInterruptError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "framework interrupt: " + Message);
}
} // namespace

void KernelModel::configureFrameworkInterruptHost() {
  using LockOwner = KernelDispatcher::WaitLockOwner;
  auto ResolveOwner = [this](uint64_t Identity,
                             bool Callback) -> llvm::Expected<LockOwner> {
    if (Callback)
      return LockOwner{LockOwner::Kind::Interrupt, Identity};
    auto Thread = ExecutionThreadKeys.find(Identity);
    if (Thread == ExecutionThreadKeys.end())
      return frameworkInterruptError("passive lock lost its acquiring thread");
    return LockOwner{LockOwner::Kind::Thread, Thread->second};
  };
  KernelInterrupts::PassiveLockHost PassiveHost;
  PassiveHost.Available = [this](uint64_t Storage,
                                 std::optional<uint64_t> Token) {
    return Dispatcher.waitLockAvailable(
        Storage,
        Token ? std::optional<LockOwner>{{LockOwner::Kind::Interrupt, *Token}}
              : std::nullopt);
  };
  PassiveHost.Acquire = [this,
                         ResolveOwner](uint64_t Storage, uint64_t Identity,
                                       bool Callback) -> llvm::Expected<bool> {
    auto Owner = ResolveOwner(Identity, Callback);
    if (!Owner)
      return Owner.takeError();
    return Dispatcher.tryAcquireWaitLock(Storage, *Owner);
  };
  PassiveHost.Release = [this, ResolveOwner](uint64_t Storage,
                                             uint64_t Identity,
                                             bool Callback) -> llvm::Error {
    auto Owner = ResolveOwner(Identity, Callback);
    if (!Owner)
      return Owner.takeError();
    return Dispatcher.releaseWaitLock(Storage, *Owner);
  };
  Interrupts.setPassiveLockHost(std::move(PassiveHost));
  using Selection = KernelFramework::InterruptSelection;
  using Connection = KernelFramework::InterruptConnection;
  using ConnectionResult = llvm::Expected<std::optional<Connection>>;
  KernelFramework::InterruptHost Host;
  auto Select = [this](const Selection &Input)
      -> llvm::Expected<std::optional<KernelInterrupts::Connection>> {
    const auto *Device = Resources.find(Input.PDO);
    if (!Device) {
      if (isProviderDevice(Input.PDO) && !Input.ResourceIndex)
        return std::optional<KernelInterrupts::Connection>{};
      return frameworkInterruptError("selection requires a resource provider");
    }
    if (!Device->Assigned || !Device->Present)
      return frameworkInterruptError("selection requires assigned resources");
    if (Input.ResourceIndex &&
        *Input.ResourceIndex >= Device->Interrupts.size())
      return frameworkInterruptError(
          "descriptor is outside assigned resources");
    size_t Count = 0;
    for (size_t I = 0; I < Device->Interrupts.size(); ++I)
      if (!Input.ResourceIndex || *Input.ResourceIndex == I)
        Count += std::max(size_t(1), Device->Interrupts[I].Messages.size());
    const size_t Ordinal =
        Input.ResourceIndex ? Input.MessageOrdinal : Input.Ordinal;
    if (Ordinal >= Count)
      return std::optional<KernelInterrupts::Connection>{};
    auto Selected =
        Interrupts.matchOrdinal(Input.PDO, Ordinal, Input.ResourceIndex);
    if (!Selected)
      return Selected.takeError();
    const auto &Resource = Device->Interrupts[Selected->ResourceIndex];
    if (Input.ShareVector &&
        *Input.ShareVector != (Resource.Share == DriverInterruptShare::Shared))
      return frameworkInterruptError("sharing differs from assigned resource");
    if (Input.CanWake && (!Input.Passive || !Resource.WakeCapable))
      return frameworkInterruptError(
          "wake interrupt requires a passive, wake-capable assigned resource");
    Selected->WakeCapable = Input.CanWake;
    Selected->Passive = Input.Passive;
    Selected->SpinLock = Input.SpinLock;
    Selected->WaitLock = Input.WaitLock;
    Selected->SynchronizeIRQL = Input.Passive ? scheduler::PassiveLevel
                                : Input.SynchronizeIRQL ? Input.SynchronizeIRQL
                                                        : Selected->IRQL;
    Selected->Version = Selected->ResourceMessage
                            ? (Input.Passive ? interrupts::MessageBasedPassive
                                             : interrupts::MessageBased)
                            : interrupts::FullySpecified;
    return std::optional<KernelInterrupts::Connection>{std::move(*Selected)};
  };
  auto Describe = [this](const KernelInterrupts::Connection &Selected) {
    const auto Fact = Interrupts.assignment(Selected);
    const auto &Resource =
        Resources.find(Selected.PDO)->Interrupts[Selected.ResourceIndex];
    Connection Info;
    Info.Token = Selected.Object;
    Info.Affinity = Fact.TranslatedAffinity;
    Info.Vector = Fact.TranslatedVector;
    Info.MessageID = Selected.MessageID.value_or(0);
    Info.Polarity = uint32_t(Fact.Polarity);
    Info.Mode = uint32_t(Resource.Mode);
    Info.IRQL = Selected.Passive ? scheduler::PassiveLevel : Selected.IRQL;
    Info.SynchronizeIRQL = Selected.SynchronizeIRQL;
    Info.Message = Selected.ResourceMessage.has_value();
    Info.Share = Resource.Share == DriverInterruptShare::Shared;
    return Info;
  };
  Host.Describe = [Select,
                   Describe](const Selection &Input) -> ConnectionResult {
    auto Selected = Select(Input);
    if (!Selected)
      return Selected.takeError();
    if (!*Selected)
      return std::optional<Connection>{};
    return std::optional<Connection>{Describe(**Selected)};
  };
  Host.Connect = [this, Select, Describe](const Selection &Input,
                                          uint64_t Handle,
                                          uint64_t ISR) -> ConnectionResult {
    auto Selected = Select(Input);
    if (!Selected)
      return Selected.takeError();
    if (!*Selected)
      return std::optional<Connection>{};
    auto &Candidate = **Selected;
    if (Candidate.SpinLock && ExecutiveSpinLocks.contains(Candidate.SpinLock))
      return frameworkInterruptError("interrupt spin lock is already held");
    const uint64_t Aligned = (NextAllocation + interrupts::TokenSize - 1) &
                             ~(interrupts::TokenSize - 1);
    if (Aligned > AllocationEnd ||
        interrupts::TokenSize > AllocationEnd - Aligned)
      return frameworkInterruptError("connection storage exhausted");
    Candidate.Object = Aligned;
    Candidate.Routine = ISR;
    Candidate.ServiceArguments =
        std::vector<uint64_t>{Handle, Candidate.MessageID.value_or(0)};
    if (auto E = Interrupts.canConnect(Candidate))
      return E;
    auto Storage = allocate(interrupts::TokenSize);
    if (!Storage)
      return Storage.takeError();
    if (*Storage != Candidate.Object)
      return frameworkInterruptError("connection allocation changed identity");
    if (auto E = Interrupts.connect(Candidate))
      return E;
    return std::optional<Connection>{Describe(Candidate)};
  };
  Host.Disconnect = [this](uint64_t Object) {
    return Interrupts.disconnectConnection(Object);
  };
  Host.SetActive = [this](uint64_t Object, bool Active) {
    return Interrupts.setActive(Object, Active);
  };
  Host.PrepareCall =
      [this](uint64_t Object, uint64_t Routine,
             llvm::ArrayRef<uint64_t> Arguments,
             uint64_t Continuation) -> llvm::Expected<GuestCallToken> {
    const auto *Connection = Interrupts.connection(Object);
    if (!Connection || !Continuation ||
        CurrentIRQL > Connection->SynchronizeIRQL ||
        Arguments.size() > scheduler::MaxArguments)
      return frameworkInterruptError(
          "callback lost its connection or caller IRQL");
    if (PendingWait)
      return frameworkInterruptError("callback cannot replace a deferred wait");
    auto Call = Interrupts.synchronize(Object, Routine, 0);
    if (!Call)
      return Call.takeError();
    FrameworkInterruptContinuations.emplace(Call->Token.ID, Continuation);
    if (Connection->Passive) {
      auto Reserved = Interrupts.reserveSynchronization(Call->Token.ID);
      if (!Reserved)
        return Reserved.takeError();
      if (!*Reserved) {
        Wait Pending;
        Pending.Type = Wait::Kind::InterruptSynchronization;
        Pending.Object = Call->Token.ID;
        Pending.Execution = CurrentExecution;
        Pending.IRQL = CurrentIRQL;
        PendingWait = Pending;
      }
    }
    return Call->Token;
  };
  Host.Acquire = [this](uint64_t Object) -> llvm::Error {
    const auto *Connection = Interrupts.connection(Object);
    if (!Connection || PendingWait)
      return frameworkInterruptError(
          "lock requires a live connection and caller");
    if (Connection->Passive) {
      if (CurrentIRQL != scheduler::PassiveLevel)
        return frameworkInterruptError("passive lock requires PASSIVE_LEVEL");
      if (!CurrentThreadKey || ApcStates[CurrentThreadKey].CriticalDepth ==
                                   profile::MaxAPCRegionNesting)
        return frameworkInterruptError(
            "passive lock lost its thread APC region");
      if (FrameworkPassiveLockThreads.contains({CurrentExecution, Object}))
        return frameworkInterruptError(
            "passive lock acquisition is already pending");
      auto Acquired = Interrupts.tryAcquirePassive(Object, CurrentExecution);
      if (!Acquired)
        return Acquired.takeError();
      ++ApcStates[CurrentThreadKey].CriticalDepth;
      FrameworkPassiveLockThreads.emplace(
          std::make_pair(CurrentExecution, Object), CurrentThreadKey);
      if (!*Acquired) {
        Wait Pending;
        Pending.Type = Wait::Kind::FrameworkInterruptLock;
        Pending.Object = Object;
        Pending.Execution = CurrentExecution;
        Pending.IRQL = CurrentIRQL;
        PendingWait = Pending;
        return llvm::Error::success();
      }
    } else {
      auto OldIRQL = Interrupts.acquire(Object, CurrentExecution, CurrentIRQL);
      if (!OldIRQL)
        return OldIRQL.takeError();
    }
    FrameworkInterruptLocks.emplace(std::make_pair(CurrentExecution, Object),
                                    CurrentIRQL);
    CurrentIRQL = Connection->SynchronizeIRQL;
    return llvm::Error::success();
  };
  Host.Release = [this](uint64_t Object) -> llvm::Error {
    const auto Saved = FrameworkInterruptLocks.find({CurrentExecution, Object});
    if (Saved == FrameworkInterruptLocks.end())
      return frameworkInterruptError(
          "release requires the acquiring execution");
    const auto Thread =
        FrameworkPassiveLockThreads.find({CurrentExecution, Object});
    if (Thread != FrameworkPassiveLockThreads.end() &&
        (Thread->second != CurrentThreadKey ||
         !ApcStates[Thread->second].CriticalDepth))
      return frameworkInterruptError(
          "passive release lost its thread APC region");
    auto Restored = Interrupts.release(Object, CurrentExecution, Saved->second,
                                       CurrentIRQL);
    if (!Restored)
      return Restored.takeError();
    CurrentIRQL = *Restored;
    FrameworkInterruptLocks.erase(Saved);
    if (Thread != FrameworkPassiveLockThreads.end()) {
      --ApcStates[Thread->second].CriticalDepth;
      FrameworkPassiveLockThreads.erase(Thread);
    }
    return llvm::Error::success();
  };
  Host.QueueDeferred =
      [this](uint64_t Handle, uint64_t Owner, uint64_t Routine,
             llvm::ArrayRef<uint64_t> Arguments, bool WorkItem,
             uint64_t Continuation,
             uint64_t SynchronizationObject) -> llvm::Expected<bool> {
    if (!Continuation || !Devices.contains(Owner) ||
        Devices.at(Owner).DeletePending)
      return frameworkInterruptError("deferred callback lost its live device");
    KernelScheduler::Callback Call{Handle,
                                   Owner,
                                   profile::WorkerThreadIdentity,
                                   Routine,
                                   {Arguments.begin(), Arguments.end()}};
    Call.SynchronizationObject = SynchronizationObject;
    auto ID = Scheduler.queueFrameworkInterrupt(std::move(Call), WorkItem);
    if (!ID)
      return ID.takeError();
    if (!*ID)
      return false;
    ScheduledModelContinuations.emplace(
        **ID, GuestCallToken{GuestCallOwner::Framework, Continuation});
    return true;
  };
  Host.HasDeferred = [this](uint64_t Handle) {
    return Scheduler.hasFrameworkInterrupt(Handle);
  };
  Host.HasPendingWake = [this](uint64_t PDO) {
    return Interrupts.hasPendingWake(PDO);
  };
  KernelInterrupts::WakeHost Wake;
  Wake.Request = [this](uint64_t PDO) {
    return Framework->powerPolicyWake(PDO);
  };
  Wake.Ready = [this](uint64_t PDO) {
    return Framework && Framework->canDeliverWakeInterrupt(PDO);
  };
  Interrupts.setWakeHost(std::move(Wake));
  Framework->setInterruptHost(std::move(Host));
}
} // namespace neverd::emulation
