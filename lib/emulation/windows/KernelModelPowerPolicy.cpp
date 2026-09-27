//===- KernelModelPowerPolicy.cpp - Explicit idle/wake provider bridge ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../DriverScenario.h"
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
llvm::Error policyError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "power policy: " + Text);
}
llvm::Expected<uint64_t> componentPolicyEpoch(const KernelResources &Resources,
                                              const KernelFramework *Framework,
                                              const KernelPoFx &PoFx,
                                              uint64_t PDO) {
  const auto Handle = PoFx.handleForPDO(PDO);
  if (!Handle)
    return policyError("PoFx decision requires a live registration");
  if (PoFx.registration(*Handle)->Owner ==
      KernelPoFx::RegistrationOwner::Framework) {
    if (!Framework)
      return policyError("internal registration lost its framework owner");
    return Framework->powerPolicyEpoch(PDO);
  }
  if (const auto *Resource = Resources.find(PDO)) {
    if (!Resource->Present || !Resource->Assigned)
      return policyError("PoFx decision requires an assigned START epoch");
    return Resource->Epoch;
  }
  // Resource-free WDM devices have no resource assignment epoch. Their unique
  // captured registration is retired before a successful STOP or REMOVE.
  return 0;
}
} // namespace

void KernelModel::configureFrameworkPowerPolicyHost() {
  KernelFramework::PowerPolicyHost Host;
  Host.Now = [this] { return Scheduler.now100ns(); };
  Host.Request = [this](uint64_t Device, DevicePowerState Target,
                        KernelFramework::PowerPolicyHost::RequestMode Mode) {
    return requestFrameworkDevicePower(Device, Target, Mode);
  };
  Host.CanWake = [this](uint64_t Device,
                        bool Sleeping) -> llvm::Expected<bool> {
    auto PDO = pnpDeviceForRoute(Device);
    if (!PDO)
      return PDO.takeError();
    const auto *Provider = pnpDeviceForPDO(*PDO);
    if (!Provider || !Provider->WakeCapabilities)
      return policyError("wake policy requires explicit provider capabilities");
    return Sleeping ? Provider->WakeCapabilities->Sx
                    : Provider->WakeCapabilities->S0;
  };
  Host.ArmWake = [this](uint64_t Device, bool Sleeping) {
    return armFrameworkWake(Device, Sleeping);
  };
  Host.FinishWake = [this](uint64_t Device, bool Triggered) {
    return finishFrameworkWake(Device, Triggered);
  };
  Host.ManagedIdle = [this](uint64_t Device, bool Idle, uint64_t Timeout) {
    return setFrameworkPoFxIdle(Device, Idle, Timeout);
  };
  Host.RemoveManaged = [this](uint64_t Device) {
    return retireFrameworkPoFx(Device);
  };
  Host.ColdAllowed = [this](uint64_t Device, bool RequireWake, bool Sleeping,
                            uint32_t Exclude) -> llvm::Expected<bool> {
    auto PDO = pnpDeviceForRoute(Device);
    if (!PDO)
      return PDO.takeError();
    const auto *Resource = Resources.find(*PDO);
    if (!Resource || !Resource->D3Cold || !Resource->D3Cold->Supported ||
        Exclude == power_policy::True ||
        (Exclude == power_policy::UseDefault &&
         !Resource->D3Cold->EnabledByDefault))
      return false;
    return !RequireWake ||
           (Sleeping ? Resource->D3Cold->WakeSx : Resource->D3Cold->WakeS0);
  };
  Framework->setPowerPolicyHost(std::move(Host));
}

llvm::Error KernelModel::requestFrameworkDevicePower(
    uint64_t Device, DevicePowerState Target,
    KernelFramework::PowerPolicyHost::RequestMode Mode) {
  auto PDO = pnpDeviceForRoute(Device);
  if (!PDO)
    return PDO.takeError();
  auto *Provider = pnpDeviceForPDO(*PDO);
  if (!Provider ||
      Provider->RequestedPowerIndex >= Provider->RequestedDevicePower.size())
    return policyError(
        "device transition requires an explicit response FIFO entry");
  const auto Index = Provider->RequestedPowerIndex;
  const auto Operation = Provider->RequestedDevicePower[Index];
  if (Operation.Type != DriverPowerType::Device ||
      Operation.Minor != DevicePowerRequest::Set ||
      Operation.State != uint32_t(Target) ||
      Operation.Action != DriverPowerAction::None)
    return policyError(
        "idle transition does not match the next device power response");
  if (Mode == KernelFramework::PowerPolicyHost::RequestMode::Validate)
    return llvm::Error::success();
  DriverRequest Input;
  Input.Kind = DriverRequestKind::Power;
  Input.DeviceID = Result.PnpDevices[Provider->ResultIndex].ID;
  Input.Power = Operation;
  RequestedPower Child;
  Child.RequestDevice = Device;
  Child.ResponseIndex = Index;
  Child.Origin = DriverRequestOrigin::FrameworkPowerPolicy;
  auto Call = preparePowerRequest(Input, Result.Requests.size(), Child);
  if (!Call)
    return Call.takeError();
  ++Provider->RequestedPowerIndex;
  auto Status = dispatchPreparedPowerRequest(*Call);
  if (!Status)
    return Status.takeError();
  if (auto E = recordDispatchReturn(Call->IRP, *Status))
    return E;
  return tryFinalizePowerRequest(Call->IRP);
}

llvm::Error KernelModel::armFrameworkWake(uint64_t Device, bool Sleeping) {
  auto PDO = pnpDeviceForRoute(Device);
  if (!PDO)
    return PDO.takeError();
  const auto *Provider = pnpDeviceForPDO(*PDO);
  if (!Provider || !Provider->WakeCapabilities ||
      !(Sleeping ? Provider->WakeCapabilities->Sx
                 : Provider->WakeCapabilities->S0))
    return policyError(
        "wake arm requires the provider's explicit D3hot capability");
  if (FrameworkWakeIRPs.contains(*PDO))
    return policyError("a wait/wake IRP is already pending for the PDO");
  DriverRequest Input;
  Input.Kind = DriverRequestKind::Power;
  Input.DeviceID = Result.PnpDevices[Provider->ResultIndex].ID;
  DriverPowerOperation Operation;
  Operation.Minor = DevicePowerRequest::WaitWake;
  Operation.Type = DriverPowerType::System;
  // WAIT_WAKE names the lowest permitted system state for this wake source,
  // not a report of the current system state. S0-only capability cannot grant
  // S3.
  Operation.State = uint32_t(Sleeping ? SystemPowerState::Sleeping3
                                      : SystemPowerState::Working);
  Input.Power = Operation;
  RequestedPower Child;
  Child.RequestDevice = Device;
  Child.Origin = DriverRequestOrigin::FrameworkWaitWake;
  auto Call = preparePowerRequest(Input, Result.Requests.size(), Child);
  if (!Call)
    return Call.takeError();
  auto Status = forwardFrameworkTransitionRequest(Call->IRP);
  if (!Status)
    return Status.takeError();
  if (*Status != windows::StatusPending)
    return policyError("wait/wake provider did not retain the request");
  return recordDispatchReturn(Call->IRP, *Status);
}

llvm::Error KernelModel::finishFrameworkWake(uint64_t Device, bool Triggered) {
  auto PDO = pnpDeviceForRoute(Device);
  if (!PDO)
    return PDO.takeError();
  auto Pending = FrameworkWakeIRPs.find(*PDO);
  if (Pending == FrameworkWakeIRPs.end())
    return llvm::Error::success();
  const uint64_t IRP = Pending->second;
  auto *Request = requestForIRP(IRP);
  if (!Request || Request->Completed || !Request->PowerOperation ||
      Request->PowerOperation->Minor != DevicePowerRequest::WaitWake)
    return policyError("wait/wake completion lost its retained IRP");
  const uint32_t Status =
      Triggered ? windows::StatusSuccess : framework::RequestCancelled;
  auto Plan = planIRPCompletion(IRP, Status);
  if (!Plan)
    return Plan.takeError();
  if (Plan->PC)
    return policyError(
        "framework wait/wake cannot own a WDM completion callback");
  if (auto E = Memory.writeInteger(IRP + windows::IRPStatusOffset, Status, 4))
    return E;
  if (auto E = Memory.writeInteger(IRP + windows::IRPInformationOffset, 0, 8))
    return E;
  Request->IOStatusWritten.fill(true);
  auto &Observation = *Result.Requests[Request->ResultIndex].Power;
  Observation.BusStatus = Status;
  Observation.BusCompletedAt100ns = Scheduler.now100ns();
  FrameworkWakeIRPs.erase(Pending);
  if (auto E = completeRequest(IRP, 0))
    return E;
  return tryFinalizePowerRequest(IRP);
}

llvm::Error KernelModel::canArmPowerPolicyEvents(
    llvm::ArrayRef<DriverPowerPolicyEvent> Events) const {
  if (Events.size() > DriverPowerPolicyEventLimit - PowerPolicyEvents.size())
    return policyError("event limit exceeded");
  for (const auto &Event : Events) {
    if (auto E = validateDriverPowerPolicyEvent(Event))
      return E;
    auto Provider = PnpDevices.find(Event.DeviceID);
    if (Provider == PnpDevices.end() || !Provider->second.AddDeviceStatus ||
        (*Provider->second.AddDeviceStatus & profile::NTStatusFailureMask))
      return policyError("event requires a successfully added device");
    if (isPoFxPowerPolicyAction(Event.Action)) {
      const uint64_t PDO = Provider->second.PDO;
      const auto Handle = PoFx.handleForPDO(PDO);
      auto State = Lifecycle.snapshot(PDO);
      if (!State)
        return State.takeError();
      if (State->Pnp != DevicePnpState::Started || !Handle)
        return policyError("PoFx decision requires a registered START epoch");
      auto Epoch = componentPolicyEpoch(Resources, Framework.get(), PoFx, PDO);
      if (!Epoch)
        return Epoch.takeError();
      if (Event.Action == DriverPowerPolicyAction::ComponentIdleState) {
        const auto &Components = PoFx.registration(*Handle)->Components;
        if (*Event.Component >= Components.size() ||
            *Event.State >= Components[*Event.Component].IdleStates.size())
          return policyError(
              "PoFx decision names an unknown component or state");
      }
    } else {
      if (!Framework || !Provider->second.FrameworkAdd)
        return policyError(
            "event requires a successfully added framework device");
      auto Epoch = Framework->powerPolicyEpoch(Provider->second.PDO);
      if (!Epoch)
        return Epoch.takeError();
    }
    if (Event.After100ns > uint64_t(INT64_MAX) - Scheduler.now100ns())
      return policyError("event deadline exceeds the virtual clock range");
    if (Event.Action == DriverPowerPolicyAction::Wake &&
        !Provider->second.WakeCapabilities)
      return policyError("wake event requires explicit provider capabilities");
  }
  return llvm::Error::success();
}
llvm::Error
KernelModel::armPowerPolicyEvents(llvm::ArrayRef<DriverPowerPolicyEvent> Events,
                                  size_t SourceIndex) {
  if (auto E = canArmPowerPolicyEvents(Events))
    return E;
  for (size_t I = 0; I < Events.size(); ++I) {
    const auto &Event = Events[I];
    const auto &Provider = PnpDevices.at(Event.DeviceID);
    uint64_t Epoch = 0, Handle = 0;
    if (isPoFxPowerPolicyAction(Event.Action)) {
      auto Captured =
          componentPolicyEpoch(Resources, Framework.get(), PoFx, Provider.PDO);
      if (!Captured)
        return Captured.takeError();
      Epoch = *Captured;
      Handle = *PoFx.handleForPDO(Provider.PDO);
    } else {
      auto Captured = Framework->powerPolicyEpoch(Provider.PDO);
      if (!Captured)
        return Captured.takeError();
      Epoch = *Captured;
    }
    PowerPolicyEvents.push_back(
        {Provider.PDO, Epoch, Result.PowerPolicyEvents.size(), Handle});
    Result.PowerPolicyEvents.push_back(
        {uint32_t(SourceIndex), uint32_t(I), Event.DeviceID, Event.Action,
         Scheduler.now100ns() + Event.After100ns, std::nullopt, Epoch,
         Event.Component, Event.State});
  }
  return llvm::Error::success();
}
llvm::Error KernelModel::processPowerPolicyEvents() {
  for (auto &Event : PowerPolicyEvents) {
    auto &Observation = Result.PowerPolicyEvents[Event.ResultIndex];
    if (Observation.OccurredAt100ns ||
        Observation.DueAt100ns > Scheduler.now100ns())
      continue;
    if (!isProviderDevice(Event.PDO))
      return policyError("event reached a retired provider");
    if (isPoFxPowerPolicyAction(Observation.Action)) {
      auto Epoch =
          componentPolicyEpoch(Resources, Framework.get(), PoFx, Event.PDO);
      if (!Epoch)
        return Epoch.takeError();
      auto State = Lifecycle.snapshot(Event.PDO);
      if (!State)
        return State.takeError();
      if (State->Pnp != DevicePnpState::Started || *Epoch != Event.Epoch)
        return policyError("event belongs to an earlier START epoch");
      if (PoFx.handleForPDO(Event.PDO) != Event.PoFxHandle)
        return policyError("event belongs to a retired PoFx registration");
      const auto *Resource = Resources.find(Event.PDO);
      if (State->DevicePower != DevicePowerState::D0 ||
          (Resource &&
           (Resource->Power != DevicePowerState::D0 || Resource->Cold)))
        return policyError(
            "PoFx decision requires a physically powered device");
      if (auto E = PoFx.process(Scheduler.now100ns()))
        return E;
    } else {
      if (!Framework)
        return policyError("framework event lost its power-policy owner");
      auto Epoch = Framework->powerPolicyEpoch(Event.PDO);
      if (!Epoch)
        return Epoch.takeError();
      if (*Epoch != Event.Epoch)
        return policyError("event belongs to an earlier START epoch");
    }
    llvm::Error E = llvm::Error::success();
    switch (Observation.Action) {
    case DriverPowerPolicyAction::Idle:
      E = Framework->powerPolicyIdle(Event.PDO);
      break;
    case DriverPowerPolicyAction::Active:
      E = Framework->powerPolicyActive(Event.PDO);
      break;
    case DriverPowerPolicyAction::Wake:
      E = Framework->powerPolicyWake(Event.PDO);
      break;
    case DriverPowerPolicyAction::ComponentIdleState:
      E = PoFx.requestIdleState(Event.PoFxHandle, *Observation.Component,
                                *Observation.State);
      break;
    case DriverPowerPolicyAction::PowerNotRequired:
      E = PoFx.requestDevicePowerNotRequired(Event.PoFxHandle);
      break;
    }
    if (E)
      return E;
    Observation.OccurredAt100ns = Scheduler.now100ns();
  }
  if (auto E = queuePoFxCallbacks())
    return E;
  if (!Framework)
    return llvm::Error::success();
  if (auto E = Framework->processPowerPolicy())
    return E;
  return completeFrameworkTransitionIfReady();
}
std::optional<uint64_t> KernelModel::nextPowerPolicyEventTime() const {
  std::optional<uint64_t> Next =
      Framework ? Framework->nextPowerPolicyTime() : std::nullopt;
  for (const auto &Event : PowerPolicyEvents) {
    const auto &Observation = Result.PowerPolicyEvents[Event.ResultIndex];
    if (!Observation.OccurredAt100ns &&
        (!Next || Observation.DueAt100ns < *Next))
      Next = Observation.DueAt100ns;
  }
  return Next;
}
bool KernelModel::hasPendingPowerPolicyEvents() const {
  return std::any_of(
      PowerPolicyEvents.begin(), PowerPolicyEvents.end(),
      [&](const auto &Event) {
        return !Result.PowerPolicyEvents[Event.ResultIndex].OccurredAt100ns;
      });
}
} // namespace neverd::emulation
