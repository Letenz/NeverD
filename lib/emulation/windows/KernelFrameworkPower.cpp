//===- KernelFrameworkPower.cpp - KMDF PnP and device power callbacks -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Sequence framework callbacks around the original PnP/power IRP. The WDM
/// host retains provider, hardware and packet ownership; this layer owns only
/// framework queue and callback lifetimes.
///
//===----------------------------------------------------------------------===//

#include "KernelFramework.h"
#include "WindowsKernelLayout.h"

#include "neverd/emulation/DriverProfile.h"

#include <algorithm>
#include <utility>

namespace neverd::emulation {
namespace {
using namespace framework;

llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "KMDF: " + Message);
}

template <typename... Errors> llvm::Error joinedErrors(Errors... Values) {
  llvm::Error Result = llvm::Error::success();
  ((Result = llvm::joinErrors(std::move(Result), std::move(Values))), ...);
  return Result;
}
} // namespace

llvm::Expected<bool>
KernelFramework::ownsPowerPolicy(uint64_t WdmDevice) const {
  const auto Device = std::find_if(
      Devices.begin(), Devices.end(), [WdmDevice](const auto &Entry) {
        return Entry.second.Wdm == WdmDevice && Entry.second.PDO;
      });
  if (Device == Devices.end())
    return invalid("power policy requires a live PnP framework device");
  return Device->second.PowerPolicyOwner;
}

llvm::Expected<bool>
KernelFramework::beginPnpPreprocess(uint64_t PDO, uint64_t IRP,
                                    DevicePnpRequest Minor) {
  if (Minor == DevicePnpRequest::Stop || Minor == DevicePnpRequest::Remove ||
      Minor == DevicePnpRequest::SurpriseRemoval)
    return beginPnpPowerTransition(PDO, IRP, Minor, 0, 0, 0);
  auto Handle = PnpDeviceHandles.find(PDO);
  if (Handle == PnpDeviceHandles.end())
    return invalid("PnP preprocessing lost its framework device");
  auto Object = Objects.find(Handle->second);
  auto Device = Devices.find(Handle->second);
  if (Object == Objects.end() || Device == Devices.end() ||
      Object->second.Deleting || !Device->second.Initialized)
    return invalid("PnP preprocessing requires a live initialized device");
  if (PendingCall || !PnpTransitions.empty() || CompletedPnp)
    return invalid("another framework callback is still pending");
  PnpStep Step{PnpPhase::QueryStop};
  uint64_t Callback = 0;
  switch (Minor) {
  case DevicePnpRequest::QueryStop:
    Callback = Device->second.Callbacks.QueryStop;
    break;
  case DevicePnpRequest::QueryRemove:
    Step.Phase = PnpPhase::QueryRemove;
    Callback = Device->second.Callbacks.QueryRemove;
    break;
  default:
    return false;
  }
  if (!Callback)
    return false;
  if (NextContinuation == UINT64_MAX)
    return invalid("framework callback identity exhausted");
  const uint64_t Token = NextContinuation++;
  PnpTransition Transition{IRP, Handle->second};
  Transition.NotificationOnly = true;
  Transition.Current = Step;
  PnpTransitions.emplace(Token, std::move(Transition));
  Continuations.emplace(Token, Continuation{});
  PendingCall = GuestCall{Token, Callback, {Handle->second}};
  return true;
}

llvm::Expected<bool> KernelFramework::beginPnpPowerTransition(
    uint64_t PDO, uint64_t IRP, DevicePnpRequest Minor, uint64_t RawResources,
    uint64_t TranslatedResources, uint64_t ResourceListSize) {
  PowerTransitionKind Kind;
  switch (Minor) {
  case DevicePnpRequest::Start:
    Kind = PowerTransitionKind::Start;
    break;
  case DevicePnpRequest::Stop:
    Kind = PowerTransitionKind::Stop;
    break;
  case DevicePnpRequest::Remove:
    Kind = PowerTransitionKind::Remove;
    break;
  case DevicePnpRequest::SurpriseRemoval:
    Kind = PowerTransitionKind::SurpriseRemoval;
    break;
  default:
    return false;
  }
  return beginPowerTransition(PDO, IRP, Kind, PowerDeviceD3Final, RawResources,
                              TranslatedResources, ResourceListSize);
}

llvm::Expected<bool>
KernelFramework::beginDevicePowerTransition(uint64_t PDO, uint64_t IRP,
                                            DevicePowerState Previous,
                                            DevicePowerState Target) {
  if (!isSupportedDriverDevicePower(Previous) ||
      !isSupportedDriverDevicePower(Target))
    return invalid("device power transition requires a supported state");
  if (Previous == Target)
    return false;
  if (Previous != DevicePowerState::D0 && Target != DevicePowerState::D0)
    return invalid("transitions between low-power states require D0");
  const bool Entering = Target == DevicePowerState::D0;
  return beginPowerTransition(PDO, IRP,
                              Entering ? PowerTransitionKind::PowerUp
                                       : PowerTransitionKind::PowerDown,
                              uint32_t(Entering ? Previous : Target));
}

llvm::Expected<bool> KernelFramework::beginPowerTransition(
    uint64_t PDO, uint64_t IRP, PowerTransitionKind Kind, uint32_t PowerState,
    uint64_t RawResources, uint64_t TranslatedResources,
    uint64_t ResourceListSize) {
  auto Handle = PnpDeviceHandles.find(PDO);
  if (Handle == PnpDeviceHandles.end())
    return invalid("PnP power transition lost its framework device");
  auto Object = Objects.find(Handle->second);
  auto Device = Devices.find(Handle->second);
  if (Object == Objects.end() || Device == Devices.end() ||
      Object->second.Deleting || !Device->second.Initialized)
    return invalid("PnP power transition requires a live initialized device");
  if (PendingCall || !PnpTransitions.empty() || CompletedPnp)
    return invalid("another framework callback is still pending");

  const bool Starting = Kind == PowerTransitionKind::Start;
  const bool Entering = Starting || Kind == PowerTransitionKind::PowerUp;
  const bool Leaving = !Entering;
  const bool ReleasesHardware = Kind != PowerTransitionKind::PowerUp &&
                                Kind != PowerTransitionKind::PowerDown;
  auto &D = Device->second;
  if (Starting && D.Policy.Epoch == UINT64_MAX)
    return invalid("power-policy START epoch exhausted");
  if (Starting && (D.InD0 || D.HardwarePrepared))
    return invalid("PnP START reached an already prepared device");
  if (Entering && (D.SelfManagedIo == SelfManagedIoState::Flushed ||
                   D.SelfManagedIo == SelfManagedIoState::Cleaned))
    return invalid("PnP START reached a device whose self-managed I/O ended");
  if (!ReleasesHardware && (!D.HardwarePrepared || D.InD0 == Entering))
    return invalid("device power transition disagrees with framework D0 state");
  PnpTransition Transition{IRP, Handle->second, Entering,
                           Kind == PowerTransitionKind::Remove ||
                               Kind == PowerTransitionKind::SurpriseRemoval};
  Transition.ReleasesHardware = ReleasesHardware;
  Transition.PowerState = PowerState;
  Transition.SuspendAfterQueues = Kind == PowerTransitionKind::SurpriseRemoval;
  if (Kind == PowerTransitionKind::SurpriseRemoval &&
      D.Callbacks.SurpriseRemoval)
    Transition.Remaining.push_back({PnpPhase::SurpriseRemoval});
  const bool SuspendSelfManaged =
      Leaving && D.SelfManagedIo == SelfManagedIoState::Running &&
      D.Callbacks.SelfManagedIoSuspend;
  if (SuspendSelfManaged && !Transition.SuspendAfterQueues)
    Transition.Remaining.push_back({PnpPhase::SelfManagedIoSuspend});
  std::vector<PnpStep> Resumes;
  for (const auto &[QueueHandle, Queue] : Queues) {
    if (Queue.Device != Handle->second || !Queue.PowerManaged)
      continue;
    for (const auto &[RequestHandle, Request] : Requests) {
      if (Request.Queue != QueueHandle || Request.Completed)
        continue;
      if (Entering && Request.PowerSuspended) {
        if (!Queue.IoResume)
          return invalid("suspended request has no EvtIoResume callback");
        Resumes.push_back({PnpPhase::IoResume, QueueHandle, RequestHandle});
      }
      if (!Leaving || Request.Queued || !D.InD0)
        continue;
      if (Request.PowerSuspended)
        return invalid("power transition found an already suspended request");
      if (Request.InCallerContext)
        return invalid("power transition found a caller-context request "
                       "outside queue ownership");
      if (!Queue.IoStop) {
        Transition.WaitingRequests.insert(RequestHandle);
        continue;
      }
      Transition.Remaining.push_back(
          {PnpPhase::IoStop, QueueHandle, RequestHandle});
    }
  }
  if (SuspendSelfManaged && Transition.SuspendAfterQueues)
    Transition.Remaining.push_back({PnpPhase::SelfManagedIoSuspend});
  auto &Policy = D.Policy;
  const bool ArmS0 = !Entering && !ReleasesHardware && Policy.IdlePowerDown &&
                     Policy.Idle && Policy.Idle->Enabled &&
                     Policy.Idle->CanWake &&
                     Policy.Armed == KernelPowerPolicy::WakeSource::None;
  if (!Entering && !ReleasesHardware && Policy.SystemSleeping && Policy.Wake) {
    auto Children = armedWakeChildren(Handle->second);
    if (!Children)
      return Children.takeError();
    Transition.WakeChildren = std::move(*Children);
    Transition.DeviceWakeEnabled = Policy.Wake->Enabled;
    Transition.ChildrenArmedForWake =
        Policy.Wake->ArmForChildren && !Transition.WakeChildren.empty();
  }
  const bool ArmSx =
      Transition.DeviceWakeEnabled || Transition.ChildrenArmedForWake;
  if (ArmSx)
    if (auto E = validateWakeEnrollment(Handle->second))
      return E;
  if (ArmS0)
    Transition.Remaining.push_back({PnpPhase::ArmWakeFromS0});
  if (ArmSx)
    Transition.Remaining.push_back({Policy.Events.ArmWakeFromSxWithReason
                                        ? PnpPhase::ArmWakeFromSxWithReason
                                        : PnpPhase::ArmWakeFromSx});

  if (Entering) {
    if (Starting && D.Callbacks.PrepareHardware)
      Transition.Remaining.push_back({PnpPhase::PrepareHardware});
    if (D.Callbacks.D0Entry)
      Transition.Remaining.push_back({PnpPhase::D0Entry});
    Transition.Remaining.push_back({PnpPhase::WakeInterrupts});
    const bool InterruptWake = hasPendingWakeInterrupts(Handle->second);
    const auto AppendWakeCallbacks = [&] {
      if (Policy.Armed == KernelPowerPolicy::WakeSource::None)
        return;
      const bool S0 = Policy.Armed == KernelPowerPolicy::WakeSource::S0;
      const PnpStep Disarm{S0 ? PnpPhase::DisarmWakeFromS0
                              : PnpPhase::DisarmWakeFromSx};
      if (InterruptWake)
        Transition.Remaining.push_back(Disarm);
      if (Policy.WakeTriggered && (S0 ? Policy.Events.WakeFromS0Triggered
                                      : Policy.Events.WakeFromSxTriggered))
        Transition.Remaining.push_back({S0 ? PnpPhase::WakeFromS0Triggered
                                           : PnpPhase::WakeFromSxTriggered});
      if (!InterruptWake)
        Transition.Remaining.push_back(Disarm);
    };
    if (InterruptWake)
      AppendWakeCallbacks();
    Transition.Remaining.push_back({PnpPhase::EnableInterrupts});
    if (D.Callbacks.D0EntryPostInterruptsEnabled)
      Transition.Remaining.push_back({PnpPhase::D0EntryPostInterruptsEnabled});
    if (!InterruptWake)
      AppendWakeCallbacks();
    Transition.Remaining.insert(Transition.Remaining.end(), Resumes.begin(),
                                Resumes.end());
    if (D.SelfManagedIo == SelfManagedIoState::Uninitialized &&
        D.Callbacks.SelfManagedIoInit)
      Transition.Remaining.push_back({PnpPhase::SelfManagedIoInit});
    else if (D.SelfManagedIo == SelfManagedIoState::Suspended &&
             D.Callbacks.SelfManagedIoSuspend &&
             D.Callbacks.SelfManagedIoRestart)
      Transition.Remaining.push_back({PnpPhase::SelfManagedIoRestart});
    if (Starting) {
      Transition.Remaining.push_back({PnpPhase::PoFxRegister});
      Transition.Remaining.push_back({PnpPhase::PoFxStart});
    }
  } else {
    if (ReleasesHardware)
      Transition.Remaining.push_back({PnpPhase::PoFxQuiesce});
    if (D.InD0 && D.Callbacks.D0ExitPreInterruptsDisabled)
      Transition.Remaining.push_back({PnpPhase::D0ExitPreInterruptsDisabled});
    Transition.Remaining.push_back({PnpPhase::DisableInterrupts});
    Transition.Remaining.push_back({PnpPhase::DrainInterrupts});
    if (D.InD0 && D.Callbacks.D0Exit)
      Transition.Remaining.push_back({PnpPhase::D0Exit});
    if (ReleasesHardware && Policy.Armed != KernelPowerPolicy::WakeSource::None)
      Transition.Remaining.push_back(
          {Policy.Armed == KernelPowerPolicy::WakeSource::S0
               ? PnpPhase::DisarmWakeFromS0
               : PnpPhase::DisarmWakeFromSx});
    if (ReleasesHardware)
      Transition.Remaining.push_back({PnpPhase::PoFxUnregister});
    if (ReleasesHardware && D.HardwarePrepared && D.Callbacks.ReleaseHardware)
      Transition.Remaining.push_back({PnpPhase::ReleaseHardware});
    if (Transition.Removing &&
        (D.SelfManagedIo == SelfManagedIoState::Running ||
         D.SelfManagedIo == SelfManagedIoState::Suspended) &&
        D.Callbacks.SelfManagedIoFlush)
      Transition.Remaining.push_back({PnpPhase::SelfManagedIoFlush});
  }
  if ((!Transition.Remaining.empty() || !Transition.WaitingRequests.empty()) &&
      NextContinuation == UINT64_MAX)
    return invalid("framework callback identity exhausted");

  if (ReleasesHardware && !Starting) {
    if (auto E = cancelUsbIdle(Handle->second))
      return E;
    if (D.Policy.UsbIdle)
      return invalid("hardware release must drain its USB idle callback");
  }
  if (Leaving && ReleasesHardware && D.PoFxHandle) {
    if (!PowerFrameworkHost.Quiesce)
      return invalid("hardware teardown lost its PoFx quiesce host");
    if (auto E = PowerFrameworkHost.Quiesce(D.PoFxHandle))
      return E;
  }
  if (Leaving)
    D.PowerQueuesHeld = true;

  if (Starting) {
    if (D.RawResources.Handle || D.TranslatedResources.Handle)
      return invalid("previous hardware resource lists remain live");
    auto Raw = createResourceList(RawResources, ResourceListSize);
    if (!Raw)
      return Raw.takeError();
    auto Translated = createResourceList(TranslatedResources, ResourceListSize);
    if (!Translated) {
      auto E = Translated.takeError();
      return llvm::joinErrors(std::move(E), retireResourceList(*Raw));
    }
    if (Raw->Count != Translated->Count) {
      auto E = invalid("raw and translated resource counts differ");
      return joinedErrors(std::move(E), retireResourceList(*Raw),
                          retireResourceList(*Translated));
    }
    D.RawResources = *Raw;
    D.TranslatedResources = *Translated;
    D.ResourcesActive = true;
    if (!D.Callbacks.PrepareHardware)
      D.HardwarePrepared = true;
  }
  const uint64_t Token = NextContinuation++;
  Continuations.emplace(Token, Continuation{});
  PnpTransitions.emplace(Token, std::move(Transition));
  auto &Active = PnpTransitions.at(Token);
  if (!Active.WaitingRequests.empty() && !Active.nextPrecedesRequestDrain()) {
    Active.WaitingForRequests = true;
    return true;
  }
  if (auto E = schedulePnpCallback(Token))
    return E;
  if (Active.CallbacksComplete) {
    auto Next = advance(Token);
    if (!Next)
      return Next.takeError();
    if (Next->has_value()) {
      PnpTransitions.erase(Token);
      return false;
    }
  }
  return true;
}

void KernelFramework::appendPowerQueuePresentations(
    uint64_t Device, std::vector<Step> &Steps) const {
  for (const auto &[Handle, Queue] : Queues)
    if (Queue.Device == Device && Queue.PowerManaged &&
        Queue.Dispatch != QueueDispatchManual)
      for (size_t I = 0; I < Queue.Pending.size(); ++I)
        Steps.push_back({StepKind::PresentQueue, Handle});
}

llvm::Error KernelFramework::finalizePnpCallbacks(uint64_t Token) {
  auto Transition = PnpTransitions.find(Token);
  if (Transition == PnpTransitions.end() ||
      Transition->second.CallbacksComplete)
    return invalid("PnP transition lost its callback state");
  auto Device = Devices.find(Transition->second.Device);
  if (Device == Devices.end())
    return invalid("PnP transition lost its device");
  if (Transition->second.NotificationOnly) {
    Transition->second.CallbacksComplete = true;
    return llvm::Error::success();
  }
  const bool Ready =
      Transition->second.Entering &&
      !(Transition->second.Status & profile::NTStatusFailureMask);
  if (Transition->second.ReleasesHardware)
    Device->second.HardwarePrepared = Ready;
  Device->second.InD0 = Ready;
  Device->second.PowerQueuesHeld = !Ready;
  if (Ready) {
    if (Transition->second.ReleasesHardware)
      ++Device->second.Policy.Epoch;
    Device->second.Policy.Started = true;
    Device->second.Policy.IdlePowerDown = false;
    Device->second.Policy.PowerUpRequested = false;
    Device->second.SelfManagedIo = SelfManagedIoState::Running;
  } else if (Device->second.SelfManagedIo == SelfManagedIoState::Running ||
             Device->second.SelfManagedIo == SelfManagedIoState::Suspended) {
    Device->second.SelfManagedIo = Transition->second.Removing
                                       ? SelfManagedIoState::Flushed
                                       : SelfManagedIoState::Suspended;
  }
  if (Ready) {
    if (auto E = holdForPoFxComponent(Transition->second.Device))
      return E;
    if (!Device->second.PowerQueuesHeld)
      appendPowerQueuePresentations(Transition->second.Device,
                                    Continuations.at(Token).Steps);
  } else if (Transition->second.ReleasesHardware) {
    Device->second.ResourcesActive = false;
    if (auto E = retireResourceLists(Device->second))
      return E;
  }
  if (!Ready && Transition->second.ReleasesHardware) {
    Device->second.Policy.Started = false;
    Device->second.Policy.Deadline.reset();
    Device->second.Policy.IdleSince.reset();
  }
  if (Ready) {
    if (auto E = restartIdleTimer(Transition->second.Device))
      return E;
  } else if (Transition->second.ReleasesHardware &&
             Device->second.Policy.Idle &&
             Device->second.Policy.Idle->systemManaged() &&
             PowerHost.RemoveManaged) {
    if (auto E = PowerHost.RemoveManaged(Device->second.Wdm))
      return E;
  }
  Transition->second.CallbacksComplete = true;
  return llvm::Error::success();
}

llvm::Error KernelFramework::resumePausedPnp() {
  // Dispatch-level completion releases the wait condition. The scheduler
  // resumes power callbacks after the current DPC has retired.
  if (CallbackIRQL || PendingCall)
    return llvm::Error::success();
  for (auto Transition = PnpTransitions.begin();
       Transition != PnpTransitions.end(); ++Transition) {
    auto &State = Transition->second;
    if (State.WaitingForPoFx) {
      auto Ready = canUnregisterPoFx(State.Device);
      if (!Ready)
        return Ready.takeError();
      if (!*Ready)
        continue;
    }
    if ((!State.WaitingForRequests && !State.WaitingForInterrupts &&
         !State.WaitingForWakeInterrupts && !State.WaitingForPoFx) ||
        !State.WaitingRequests.empty() ||
        (State.WaitingForInterrupts && hasDeferredInterrupts(State.Device)) ||
        (State.WaitingForWakeInterrupts &&
         hasPendingWakeInterrupts(State.Device)))
      continue;
    State.WaitingForRequests = false;
    State.WaitingForInterrupts = false;
    State.WaitingForWakeInterrupts = false;
    State.WaitingForPoFx = false;
    const uint64_t Token = Transition->first;
    if (!State.Remaining.empty()) {
      if (auto E = schedulePnpCallback(Token))
        return E;
      if (!State.CallbacksComplete)
        return llvm::Error::success();
    } else if (auto E = finalizePnpCallbacks(Token)) {
      return E;
    }
    auto Next = advance(Token);
    if (!Next)
      return Next.takeError();
    if (Next->has_value()) {
      CompletedPnp = PnpCompletion{State.IRP, State.Status};
      PnpTransitions.erase(Transition);
    }
    return llvm::Error::success();
  }
  return llvm::Error::success();
}

llvm::Error KernelFramework::schedulePnpCallback(uint64_t Token) {
  auto &Transition = PnpTransitions.at(Token);
  auto &D = Devices.at(Transition.Device);
  while (!Transition.Remaining.empty()) {
    Transition.Current = Transition.Remaining.front();
    Transition.Remaining.pop_front();
    const auto Phase = Transition.Current.Phase;
    if (Phase == PnpPhase::PoFxRegister || Phase == PnpPhase::PoFxStart ||
        Phase == PnpPhase::PoFxQuiesce || Phase == PnpPhase::PoFxUnregister) {
      auto Scheduled = advancePoFxLifecycle(Token);
      if (!Scheduled)
        return Scheduled.takeError();
      if (*Scheduled)
        return llvm::Error::success();
      continue;
    }
    if (Phase == PnpPhase::EnableInterrupts ||
        Phase == PnpPhase::DisableInterrupts) {
      Transition.CurrentInterrupt = 0;
      if (Phase == PnpPhase::EnableInterrupts)
        D.InD0 = true;
      auto Scheduled =
          advancePnpInterrupts(Token, Phase == PnpPhase::EnableInterrupts);
      if (!Scheduled)
        return Scheduled.takeError();
      if (*Scheduled)
        return llvm::Error::success();
      continue;
    }
    if (Phase == PnpPhase::DisarmWakeParents) {
      while (!Transition.WakeParentsToDisarm.empty()) {
        const uint64_t Parent = Transition.WakeParentsToDisarm.front();
        Transition.WakeParentsToDisarm.pop_front();
        const uint64_t Callback =
            Devices.at(Parent).Policy.Events.DisarmWakeFromSx;
        if (!Callback)
          continue;
        if (!Transition.WakeParentsToDisarm.empty())
          Transition.Remaining.push_front({PnpPhase::DisarmWakeParents});
        PendingCall = GuestCall{Token, Callback, {Parent}};
        return llvm::Error::success();
      }
      continue;
    }
    if (Phase == PnpPhase::WakeInterrupts) {
      D.InD0 = true;
      if (hasPendingWakeInterrupts(Transition.Device)) {
        Transition.WaitingForWakeInterrupts = true;
        return llvm::Error::success();
      }
      continue;
    }
    if (Phase == PnpPhase::DrainInterrupts) {
      if (hasDeferredInterrupts(Transition.Device)) {
        Transition.WaitingForInterrupts = true;
        return llvm::Error::success();
      }
      continue;
    }
    const bool ArmS0 = Phase == PnpPhase::ArmWakeFromS0;
    const bool ArmSx = Phase == PnpPhase::ArmWakeFromSx ||
                       Phase == PnpPhase::ArmWakeFromSxWithReason;
    const bool Disarm = Phase == PnpPhase::DisarmWakeFromS0 ||
                        Phase == PnpPhase::DisarmWakeFromSx;
    if (ArmS0 || ArmSx) {
      if (!PowerHost.ArmWake)
        return invalid("wake arm requires a provider WAIT_WAKE bridge");
      if (auto E = PowerHost.ArmWake(D.Wdm, ArmSx))
        return E;
    }
    if (Disarm) {
      if (auto E = retireChildWake(Transition.Device, Transition))
        return E;
      D.Policy.Armed = KernelPowerPolicy::WakeSource::None;
      D.Policy.ArmedForDevice = false;
      D.Policy.ArmedChildren.clear();
      D.Policy.WakeTriggered = false;
      if (!Transition.WakeParentsToDisarm.empty())
        Transition.Remaining.push_front({PnpPhase::DisarmWakeParents});
    }
    uint64_t Callback = 0;
    switch (Transition.Current.Phase) {
#define NEVERD_POWER_POLICY_CALLBACK(Name, Index, ResultKind)                  \
  case PnpPhase::Name:                                                         \
    Callback = D.Policy.Events.Name;                                           \
    break;
#include "KernelPowerPolicyCallbacks.def"
#undef NEVERD_POWER_POLICY_CALLBACK
#define NEVERD_FRAMEWORK_PNP_CALLBACK(Name, Index, Result)                     \
  case PnpPhase::Name:                                                         \
    Callback = D.Callbacks.Name;                                               \
    break;
#include "KernelFrameworkPnpCallbacks.def"
#undef NEVERD_FRAMEWORK_PNP_CALLBACK
    case PnpPhase::IoStop:
    case PnpPhase::IoResume:
    case PnpPhase::EnableInterrupts:
    case PnpPhase::DisableInterrupts:
    case PnpPhase::WakeInterrupts:
    case PnpPhase::DrainInterrupts:
    case PnpPhase::PoFxRegister:
    case PnpPhase::PoFxStart:
    case PnpPhase::PoFxQuiesce:
    case PnpPhase::PoFxUnregister:
    case PnpPhase::DisarmWakeParents:
      break;
    }
    std::vector<uint64_t> Arguments{Transition.Device};
    switch (Transition.Current.Phase) {
    case PnpPhase::EnableInterrupts:
    case PnpPhase::DisableInterrupts:
    case PnpPhase::WakeInterrupts:
    case PnpPhase::DrainInterrupts:
    case PnpPhase::PoFxRegister:
    case PnpPhase::PoFxStart:
    case PnpPhase::PoFxQuiesce:
    case PnpPhase::PoFxUnregister:
    case PnpPhase::DisarmWakeParents:
      llvm_unreachable(
          "internal interrupt phase handled before guest dispatch");
    case PnpPhase::ArmWakeFromSxWithReason:
      Arguments.push_back(Transition.DeviceWakeEnabled);
      Arguments.push_back(Transition.ChildrenArmedForWake);
      break;
    case PnpPhase::ArmWakeFromS0:
    case PnpPhase::ArmWakeFromSx:
    case PnpPhase::DisarmWakeFromS0:
    case PnpPhase::DisarmWakeFromSx:
    case PnpPhase::WakeFromS0Triggered:
    case PnpPhase::WakeFromSxTriggered:
      break;
    case PnpPhase::QueryStop:
    case PnpPhase::QueryRemove:
    case PnpPhase::SurpriseRemoval:
      break;
    case PnpPhase::SelfManagedIoInit:
    case PnpPhase::SelfManagedIoRestart:
      D.InD0 = true;
      D.PowerQueuesHeld = false;
      if (auto E = holdForPoFxComponent(Transition.Device))
        return E;
      D.SelfManagedIo = SelfManagedIoState::Running;
      break;
    case PnpPhase::SelfManagedIoSuspend:
      D.SelfManagedIo = SelfManagedIoState::Suspended;
      break;
    case PnpPhase::SelfManagedIoFlush:
      D.SelfManagedIo = SelfManagedIoState::Flushed;
      break;
    case PnpPhase::SelfManagedIoCleanup:
      D.SelfManagedIo = SelfManagedIoState::Cleaned;
      break;
    case PnpPhase::IoStop: {
      auto Queue = Queues.find(Transition.Current.Queue);
      auto Request = Requests.find(Transition.Current.Request);
      if (Queue == Queues.end() || Request == Requests.end() ||
          Request->second.Completed || Request->second.Queued)
        return invalid("I/O stop callback lost its driver-owned request");
      Callback = Queue->second.IoStop;
      uint64_t Flags = Transition.Removing ? RequestStopActionPurge
                                           : RequestStopActionSuspend;
      if (Request->second.Cancellation == CancelState::Marked)
        Flags |= RequestStopRequestCancelable;
      Arguments = {Transition.Current.Queue, Transition.Current.Request, Flags};
      break;
    }
    case PnpPhase::IoResume: {
      auto Queue = Queues.find(Transition.Current.Queue);
      auto Request = Requests.find(Transition.Current.Request);
      if (Queue == Queues.end() || Request == Requests.end() ||
          !Request->second.PowerSuspended || Request->second.Completed)
        return invalid("I/O resume callback lost its suspended request");
      Callback = Queue->second.IoResume;
      Arguments = {Transition.Current.Queue, Transition.Current.Request};
      break;
    }
    case PnpPhase::PrepareHardware:
      Arguments.push_back(D.RawResources.Handle);
      Arguments.push_back(D.TranslatedResources.Handle);
      break;
    case PnpPhase::D0Entry:
    case PnpPhase::D0EntryPostInterruptsEnabled:
    case PnpPhase::D0Exit:
    case PnpPhase::D0ExitPreInterruptsDisabled:
      Arguments.push_back(Transition.PowerState);
      break;
    case PnpPhase::ReleaseHardware:
      Arguments.push_back(D.TranslatedResources.Handle);
      break;
    }
    if (!Callback && (ArmS0 || ArmSx || Disarm)) {
      if (ArmS0 || ArmSx) {
        D.Policy.Armed = ArmS0 ? KernelPowerPolicy::WakeSource::S0
                               : KernelPowerPolicy::WakeSource::Sx;
        D.Policy.ArmedForDevice = ArmS0 || Transition.DeviceWakeEnabled;
        D.Policy.ArmedChildren = std::move(Transition.WakeChildren);
      }
      continue;
    }
    if (!Callback)
      return invalid("PnP transition lost its registered callback");
    PendingCall = GuestCall{Token, Callback, std::move(Arguments)};
    if (Phase == PnpPhase::IoStop || Phase == PnpPhase::IoResume) {
      if (auto E = retainQueueCallback(Token, Transition.Current.Queue))
        return E;
      PendingCall->SynchronizationObject =
          callbackSynchronizationObject(Transition.Current.Queue);
    }
    return llvm::Error::success();
  }
  return finalizePnpCallbacks(Token);
}

std::optional<KernelFramework::PnpCompletion>
KernelFramework::takePnpCompletion() {
  return std::exchange(CompletedPnp, std::nullopt);
}

llvm::Expected<KernelFramework::PnpCallbackProgress>
KernelFramework::finishPnpCallback(uint64_t Token, uint64_t Result) {
  auto Transition = PnpTransitions.find(Token);
  if (Transition != PnpTransitions.end()) {
    auto Device = Devices.find(Transition->second.Device);
    if (Device == Devices.end() || CompletedPnp)
      return invalid("PnP power callback lost its device or completion");
    auto &State = Transition->second;
    if (!State.CallbacksComplete) {
      const bool StoppingRequest = State.Current.Phase == PnpPhase::IoStop;
      const bool ResumingRequest = State.Current.Phase == PnpPhase::IoResume;
      enum class CallbackResult { Status, Void };
      bool VoidResult = false;
      switch (State.Current.Phase) {
#define NEVERD_POWER_POLICY_CALLBACK(Name, Index, ResultKind)                  \
  case PnpPhase::Name:                                                         \
    VoidResult = CallbackResult::ResultKind == CallbackResult::Void;           \
    break;
#include "KernelPowerPolicyCallbacks.def"
#undef NEVERD_POWER_POLICY_CALLBACK
#define NEVERD_FRAMEWORK_PNP_CALLBACK(Name, Index, Result)                     \
  case PnpPhase::Name:                                                         \
    VoidResult = CallbackResult::Result == CallbackResult::Void;               \
    break;
#include "KernelFrameworkPnpCallbacks.def"
#undef NEVERD_FRAMEWORK_PNP_CALLBACK
      case PnpPhase::EnableInterrupts:
      case PnpPhase::DisableInterrupts:
      case PnpPhase::PoFxRegister:
        break;
      case PnpPhase::WakeInterrupts:
      case PnpPhase::DrainInterrupts:
      case PnpPhase::PoFxStart:
      case PnpPhase::PoFxQuiesce:
      case PnpPhase::PoFxUnregister:
      case PnpPhase::DisarmWakeParents:
      case PnpPhase::IoStop:
      case PnpPhase::IoResume:
        VoidResult = true;
        break;
      }
      if (StoppingRequest) {
        auto Request = Requests.find(State.Current.Request);
        if (Request != Requests.end() && !Request->second.Completed) {
          if (Request->second.StopAcknowledged)
            Request->second.StopAcknowledged = false;
          else
            State.WaitingRequests.insert(State.Current.Request);
        }
      }
      if (ResumingRequest)
        if (auto Request = Requests.find(State.Current.Request);
            Request != Requests.end())
          Request->second.PowerSuspended = false;
      const uint32_t Status =
          VoidResult ? windows::StatusSuccess : uint32_t(Result);
      if (!VoidResult && Status == windows::StatusPending)
        return invalid("PnP power callback returned STATUS_PENDING");
      if (State.NotificationOnly && !VoidResult &&
          Status == windows::StatusNotSupported)
        return invalid("PnP query callback returned STATUS_NOT_SUPPORTED");
      bool Failed = Status & profile::NTStatusFailureMask;
      const bool ArmS0 = State.Current.Phase == PnpPhase::ArmWakeFromS0;
      const bool ArmSx =
          State.Current.Phase == PnpPhase::ArmWakeFromSx ||
          State.Current.Phase == PnpPhase::ArmWakeFromSxWithReason;
      if (ArmS0 || ArmSx) {
        auto &Policy = Device->second.Policy;
        if (!Failed) {
          Policy.Armed = ArmS0 ? KernelPowerPolicy::WakeSource::S0
                               : KernelPowerPolicy::WakeSource::Sx;
          Policy.ArmedForDevice = ArmS0 || State.DeviceWakeEnabled;
          Policy.ArmedChildren = std::move(State.WakeChildren);
        } else {
          if (auto E = PowerHost.FinishWake(Device->second.Wdm, false))
            return E;
          if (ArmSx) {
            State.Remaining.push_front({PnpPhase::DisarmWakeFromSx});
            Failed = false;
          } else {
            State.Remaining.clear();
            Policy.IdleSince.reset();
            Policy.Deadline.reset();
            State.Status = Status;
            State.NotificationOnly = true;
            Device->second.PowerQueuesHeld = false;
          }
        }
      }
      if (Failed && !(State.Status & profile::NTStatusFailureMask))
        State.Status = Status;
      auto &D = Device->second;
      if (State.Current.Phase == PnpPhase::PoFxUnregister)
        if (auto E = unregisterPoFx(State.Device))
          return E;
      const bool InterruptPhase =
          State.Current.Phase == PnpPhase::EnableInterrupts ||
          State.Current.Phase == PnpPhase::DisableInterrupts;
      if (InterruptPhase) {
        if (auto E = finishPnpInterrupt(Token, Status))
          return E;
        if (!Failed || State.Current.Phase == PnpPhase::DisableInterrupts) {
          auto Scheduled = advancePnpInterrupts(
              Token, State.Current.Phase == PnpPhase::EnableInterrupts);
          if (!Scheduled)
            return Scheduled.takeError();
          if (*Scheduled)
            return PnpCallbackProgress::Scheduled;
        }
      }
      if (!Failed && State.Current.Phase == PnpPhase::PrepareHardware)
        D.HardwarePrepared = true;
      if (!Failed && State.Current.Phase == PnpPhase::D0Entry)
        D.InD0 = true;
      if (State.Current.Phase == PnpPhase::D0Exit)
        D.InD0 = false;
      if (State.Current.Phase == PnpPhase::ReleaseHardware) {
        D.HardwarePrepared = false;
        D.ResourcesActive = false;
        if (auto E = retireResourceLists(D))
          return E;
      }
      if (State.Entering && Failed &&
          (State.Current.Phase == PnpPhase::EnableInterrupts ||
           State.Current.Phase == PnpPhase::PrepareHardware ||
           State.Current.Phase == PnpPhase::D0Entry ||
           State.Current.Phase == PnpPhase::D0EntryPostInterruptsEnabled ||
           State.Current.Phase == PnpPhase::SelfManagedIoInit ||
           State.Current.Phase == PnpPhase::SelfManagedIoRestart ||
           State.Current.Phase == PnpPhase::PoFxRegister)) {
        State.Remaining.clear();
        State.ReleasesHardware = true;
        D.PowerQueuesHeld = true;
        if (State.Current.Phase == PnpPhase::PoFxRegister &&
            D.SelfManagedIo == SelfManagedIoState::Running &&
            D.Callbacks.SelfManagedIoSuspend)
          State.Remaining.push_back({PnpPhase::SelfManagedIoSuspend});
        State.Remaining.push_back({PnpPhase::PoFxQuiesce});
        if (D.InD0 && D.Callbacks.D0ExitPreInterruptsDisabled)
          State.Remaining.push_back({PnpPhase::D0ExitPreInterruptsDisabled});
        State.Remaining.push_back({PnpPhase::DisableInterrupts});
        State.Remaining.push_back({PnpPhase::DrainInterrupts});
        if (D.InD0 && D.Callbacks.D0Exit)
          State.Remaining.push_back({PnpPhase::D0Exit});
        State.Remaining.push_back({PnpPhase::PoFxUnregister});
        if (D.Callbacks.ReleaseHardware)
          State.Remaining.push_back({PnpPhase::ReleaseHardware});
      }
      if (Failed && D.SelfManagedIo != SelfManagedIoState::Uninitialized &&
          (State.Current.Phase == PnpPhase::SelfManagedIoInit ||
           State.Current.Phase == PnpPhase::SelfManagedIoRestart ||
           State.Current.Phase == PnpPhase::SelfManagedIoSuspend)) {
        auto AppendOnce = [&](PnpPhase Phase, uint64_t Callback) {
          if (Callback &&
              std::none_of(
                  State.Remaining.begin(), State.Remaining.end(),
                  [&](const PnpStep &Step) { return Step.Phase == Phase; }))
            State.Remaining.push_back({Phase});
        };
        if (D.SelfManagedIo != SelfManagedIoState::Flushed &&
            D.SelfManagedIo != SelfManagedIoState::Cleaned)
          AppendOnce(PnpPhase::SelfManagedIoFlush,
                     D.Callbacks.SelfManagedIoFlush);
        if (D.SelfManagedIo != SelfManagedIoState::Cleaned)
          AppendOnce(PnpPhase::SelfManagedIoCleanup,
                     D.Callbacks.SelfManagedIoCleanup);
      }
      if (State.nextPrecedesRequestDrain()) {
        if (auto E = schedulePnpCallback(Token))
          return E;
        return State.CallbacksComplete ? PnpCallbackProgress::Continue
               : (State.WaitingForInterrupts ||
                  State.WaitingForWakeInterrupts || State.WaitingForPoFx)
                   ? PnpCallbackProgress::Waiting
                   : PnpCallbackProgress::Scheduled;
      }
      if (!State.WaitingRequests.empty()) {
        State.WaitingForRequests = true;
        return PnpCallbackProgress::Waiting;
      }
      if (!State.Remaining.empty()) {
        if (auto E = schedulePnpCallback(Token))
          return E;
        return State.CallbacksComplete ? PnpCallbackProgress::Continue
               : (State.WaitingForInterrupts ||
                  State.WaitingForWakeInterrupts || State.WaitingForPoFx)
                   ? PnpCallbackProgress::Waiting
                   : PnpCallbackProgress::Scheduled;
      }
      if (auto E = finalizePnpCallbacks(Token))
        return E;
    }
  }
  return PnpCallbackProgress::Continue;
}

} // namespace neverd::emulation
