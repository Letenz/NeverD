//===- KernelModelPowerEvents.cpp - Captured provider power events -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Capture policy owners before scheduling external power events.
///
//===----------------------------------------------------------------------===//

#include "../DriverScenario.h"
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>
#include <type_traits>

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

llvm::Expected<KernelModel::PowerEventOwner>
KernelModel::capturePowerEventOwner(const DriverPowerPolicyEvent &Event) const {
  if (auto E = validateDriverPowerPolicyEvent(Event))
    return E;
  auto Provider = PnpDevices.find(Event.DeviceID);
  if (Provider == PnpDevices.end() || !Provider->second.AddDeviceStatus ||
      (*Provider->second.AddDeviceStatus & profile::NTStatusFailureMask))
    return policyError("event requires a successfully added device");
  const uint64_t PDO = Provider->second.PDO;
  if (!isProviderDevice(PDO))
    return policyError("event requires a live provider");
  if (Event.After100ns > uint64_t(INT64_MAX) - Scheduler.now100ns())
    return policyError("event deadline exceeds the virtual clock range");
  if (Event.Action == DriverPowerPolicyAction::UsbIdlePermission) {
    auto Epoch = deviceStartEpoch(PDO, false);
    auto Members = captureUsbIdlePermission(PDO);
    if (!Epoch || !Members)
      return llvm::joinErrors(Epoch.takeError(), Members.takeError());
    return PowerEventOwner{UsbIdlePermissionEvent{*Epoch, std::move(*Members)}};
  }
  if (isPoFxPowerPolicyAction(Event.Action)) {
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
        return policyError("PoFx decision names an unknown component or state");
    }
    return PowerEventOwner{PoFxPowerEvent{*Epoch, *Handle}};
  }
  if (Event.Action == DriverPowerPolicyAction::Wake &&
      !Provider->second.WakeCapabilities)
    return policyError("wake event requires explicit provider capabilities");
  if (Provider->second.FrameworkAdd) {
    if (!Framework)
      return policyError("event lost its framework policy owner");
    auto Epoch = Framework->powerPolicyEpoch(PDO);
    if (!Epoch)
      return Epoch.takeError();
    return PowerEventOwner{FrameworkPowerEvent{*Epoch}};
  }
  if (Event.Action != DriverPowerPolicyAction::Wake)
    return policyError("idle and active events require framework power policy");
  const auto Wake = ProviderWakeIRPs.find(PDO);
  if (Wake == ProviderWakeIRPs.end())
    return policyError("native wake event requires an already retained IRP");
  const auto *Request = requestForIRP(Wake->second.IRP);
  if (!Request || Request->Completed || !Request->ChildPower ||
      Request->ChildPower->Origin != DriverRequestOrigin::PoRequestPowerIrp)
    return policyError("native wake event lost its originating power request");
  auto Epoch = deviceStartEpoch(PDO, false);
  if (!Epoch)
    return Epoch.takeError();
  if (*Epoch != Wake->second.StartEpoch)
    return policyError("native wake belongs to an earlier START epoch");
  return PowerEventOwner{WdmWakeEvent{*Epoch, Wake->second.IRP}};
}

llvm::Error KernelModel::canArmPowerPolicyEvents(
    llvm::ArrayRef<DriverPowerPolicyEvent> Events) const {
  if (Events.size() > DriverPowerPolicyEventLimit - PowerPolicyEvents.size())
    return policyError("event limit exceeded");
  for (const auto &Event : Events) {
    auto Owner = capturePowerEventOwner(Event);
    if (!Owner)
      return Owner.takeError();
  }
  return llvm::Error::success();
}

llvm::Error
KernelModel::armPowerPolicyEvents(llvm::ArrayRef<DriverPowerPolicyEvent> Events,
                                  size_t SourceIndex) {
  if (Events.size() > DriverPowerPolicyEventLimit - PowerPolicyEvents.size())
    return policyError("event limit exceeded");
  std::vector<PowerEventOwner> Owners;
  for (const auto &Event : Events) {
    auto Owner = capturePowerEventOwner(Event);
    if (!Owner)
      return Owner.takeError();
    Owners.push_back(std::move(*Owner));
  }
  for (size_t I = 0; I < Events.size(); ++I) {
    const auto &Event = Events[I];
    const auto &Provider = PnpDevices.at(Event.DeviceID);
    const uint64_t Epoch = std::visit(
        [](const auto &Owner) {
          if constexpr (std::is_same_v<std::decay_t<decltype(Owner)>,
                                       WdmWakeEvent>)
            return Owner.StartEpoch;
          else
            return Owner.Epoch;
        },
        Owners[I]);
    PowerPolicyEvents.push_back(
        {Provider.PDO, Result.PowerPolicyEvents.size(), std::move(Owners[I])});
    Result.PowerPolicyEvents.push_back(
        {uint32_t(SourceIndex), uint32_t(I), Event.DeviceID, Event.Action,
         Scheduler.now100ns() + Event.After100ns, std::nullopt, Epoch,
         Event.Component, Event.State});
    if (const auto *Usb = std::get_if<UsbIdlePermissionEvent>(
            &PowerPolicyEvents.back().Owner)) {
      auto &Facts = Result.PowerPolicyEvents.back().UsbIdleMembers;
      for (const auto &Key : Usb->Members) {
        const auto *Member = pnpDeviceForPDO(Key.PDO);
        Facts.push_back({Result.PnpDevices[Member->ResultIndex].ID, Key.PDO,
                         Key.IRP, Key.StartEpoch});
      }
    }
  }
  return llvm::Error::success();
}
llvm::Error KernelModel::processPowerPolicyEvents() {
  // Reserve every due native completion before publishing any wake result.
  // Each capture owns one packet, even if its callback later rearms the PDO.
  std::map<uint64_t, IRPCompletionPlan> NativePlans;
  std::vector<KernelScheduler::Callback> Callbacks;
  std::vector<UsbIdleKey> UsbKeys;
  for (const auto &Event : PowerPolicyEvents) {
    const auto &Observation = Result.PowerPolicyEvents[Event.ResultIndex];
    if (Observation.OccurredAt100ns ||
        Observation.DueAt100ns > Scheduler.now100ns())
      continue;
    if (const auto *Usb = std::get_if<UsbIdlePermissionEvent>(&Event.Owner)) {
      auto Epoch = deviceStartEpoch(Event.PDO, false);
      if (!Epoch)
        return Epoch.takeError();
      if (*Epoch != Usb->Epoch)
        return policyError(
            "USB permission belongs to an earlier coordinator START");
      if (auto E = validateUsbIdlePermission(Usb->Members))
        return E;
      UsbKeys.insert(UsbKeys.end(), Usb->Members.begin(), Usb->Members.end());
      for (const auto &Key : Usb->Members) {
        const auto &Submission = *UsbIdle.submission(Key.PDO);
        Callbacks.push_back({Key.IRP,
                             Key.PDO,
                             profile::WorkerThreadIdentity,
                             Submission.Callback,
                             {Submission.Context}});
      }
      continue;
    }
    const auto *Wake = std::get_if<WdmWakeEvent>(&Event.Owner);
    if (!Wake || Observation.OccurredAt100ns ||
        Observation.DueAt100ns > Scheduler.now100ns())
      continue;
    auto Epoch = deviceStartEpoch(Event.PDO, false);
    if (!Epoch)
      return Epoch.takeError();
    if (*Epoch != Wake->StartEpoch)
      return policyError("native wake event belongs to an earlier START epoch");
    auto Retained =
        preflightProviderWake(Event.PDO, Wake->IRP, windows::StatusSuccess);
    if (!Retained)
      return Retained.takeError();
    if (NativePlans.contains(Wake->IRP))
      return policyError("due wake events cannot complete the same IRP twice");
    auto Plan = planIRPCompletion(Wake->IRP, windows::StatusSuccess);
    if (!Plan)
      return Plan.takeError();
    if (Plan->PC)
      Callbacks.push_back({Wake->IRP, Event.PDO, profile::WorkerThreadIdentity,
                           Plan->PC, Plan->Arguments});
    NativePlans.emplace(Wake->IRP, std::move(*Plan));
  }
  if (NativePlans.size() > UINT64_MAX - NextIRPCall)
    return policyError("wake completion identity exhausted");
  if (!UsbKeys.empty())
    if (auto E = UsbIdle.canQueueCallbacks(UsbKeys))
      return E;
  if (auto E = Scheduler.canEnqueueCompletions(Callbacks))
    return E;
  for (auto &Event : PowerPolicyEvents) {
    auto &Observation = Result.PowerPolicyEvents[Event.ResultIndex];
    if (Observation.OccurredAt100ns ||
        Observation.DueAt100ns > Scheduler.now100ns())
      continue;
    if (!isProviderDevice(Event.PDO))
      return policyError("event reached a retired provider");
    if (const auto *Usb = std::get_if<UsbIdlePermissionEvent>(&Event.Owner)) {
      if (auto E = queueUsbIdlePermission(Usb->Members))
        return E;
      Observation.OccurredAt100ns = Scheduler.now100ns();
      continue;
    }
    if (const auto *Wake = std::get_if<WdmWakeEvent>(&Event.Owner)) {
      const auto &Plan = NativePlans.at(Wake->IRP);
      if (auto E = completeProviderWake(Event.PDO, Wake->IRP,
                                        windows::StatusSuccess, Event.PDO))
        return E;
      auto Call = takeWdmGuestCall();
      if (bool(Call) != bool(Plan.PC) ||
          (Call && (Call->Token.Owner != GuestCallOwner::WDM ||
                    Call->PC != Plan.PC || Call->Arguments != Plan.Arguments)))
        return policyError("native wake callback changed after preflight");
      if (Call) {
        auto ID = Scheduler.enqueueWDMCompletion(
            {Wake->IRP, Event.PDO, profile::WorkerThreadIdentity, Call->PC,
             std::move(Call->Arguments)});
        if (!ID)
          return ID.takeError();
        ScheduledModelContinuations.emplace(*ID, Call->Token);
      }
      Observation.OccurredAt100ns = Scheduler.now100ns();
      continue;
    }
    if (const auto *Component = std::get_if<PoFxPowerEvent>(&Event.Owner)) {
      auto Epoch =
          componentPolicyEpoch(Resources, Framework.get(), PoFx, Event.PDO);
      if (!Epoch)
        return Epoch.takeError();
      auto State = Lifecycle.snapshot(Event.PDO);
      if (!State)
        return State.takeError();
      if (State->Pnp != DevicePnpState::Started || *Epoch != Component->Epoch)
        return policyError("event belongs to an earlier START epoch");
      if (PoFx.handleForPDO(Event.PDO) != Component->Handle)
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
      if (*Epoch != std::get<FrameworkPowerEvent>(Event.Owner).Epoch)
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
      E = PoFx.requestIdleState(std::get<PoFxPowerEvent>(Event.Owner).Handle,
                                *Observation.Component, *Observation.State);
      break;
    case DriverPowerPolicyAction::PowerNotRequired:
      E = PoFx.requestDevicePowerNotRequired(
          std::get<PoFxPowerEvent>(Event.Owner).Handle);
      break;
    case DriverPowerPolicyAction::UsbIdlePermission:
      llvm_unreachable("USB permission has its captured owner path");
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
