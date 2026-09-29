//===- KernelModelPowerPolicy.cpp - Explicit idle/wake provider bridge ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bridge explicit power policy to retained provider IRPs. A propagated wake
/// validates the complete set of captured obligations before completing any.
///
//===----------------------------------------------------------------------===//

#include "DriverScenario.h"
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
llvm::Error policyError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "power policy: " + Text);
}
namespace callback {
#define NEVERD_PROVIDER_CALLBACK(Name, Arity)                                  \
  constexpr llvm::StringLiteral Name = #Name;
#include "KernelProviderCallbacks.def"
#undef NEVERD_PROVIDER_CALLBACK
} // namespace callback
} // namespace

std::optional<unsigned>
KernelModel::providerArgumentCount(llvm::StringRef Name) {
#define NEVERD_PROVIDER_CALLBACK(Routine, Arity)                               \
  if (Name == callback::Routine)                                               \
    return Arity;
#include "KernelProviderCallbacks.def"
#undef NEVERD_PROVIDER_CALLBACK
  return std::nullopt;
}

llvm::Expected<uint64_t>
KernelModel::callProviderExport(const KernelExportRegistry::Export &Export,
                                llvm::ArrayRef<uint64_t> Arguments) {
  const auto Arity = providerArgumentCount(Export.Name);
  if (Export.Kind != KernelExportRegistry::ExportKind::ProviderFunction ||
      !Arity || Arguments.size() != *Arity)
    return policyError("unknown provider callback or invalid argument count");
  if (Export.Name == callback::CancelUsbIdle)
    return cancelUsbIdle(Export, Arguments);
  if (Export.Name != callback::CancelWaitWake)
    return policyError("unsupported provider callback");
  const uint64_t PDO = Arguments[0], IRP = Arguments[1];
  const auto Wake = ProviderWakeIRPs.find(PDO);
  const auto Call = IRPCalls.find(CurrentGuestCall.ID);
  if (Export.Binding != PDO || Wake == ProviderWakeIRPs.end() ||
      Wake->second.IRP != IRP || Wake->second.CancelRoutine != Export.Address ||
      Wake->second.FrameworkEpoch ||
      CurrentGuestCall.Owner != GuestCallOwner::WDM || Call == IRPCalls.end() ||
      Call->second.Kind != IRPCallKind::Cancel ||
      !Call->second.AwaitingCallback || Call->second.IRP != IRP ||
      !CancelLock.Callback || !CancelLock.Held || CancelLock.IRP != IRP ||
      CancelLock.Owner != CurrentExecution)
    return policyError("provider cancel callback requires its active owner");
  if (auto E = releaseCancelSpinLock(CancelLock.OldIRQL))
    return std::move(E);
  if (auto E = completeProviderWake(PDO, IRP, framework::RequestCancelled,
                                    std::nullopt))
    return std::move(E);
  return 0;
}

llvm::Expected<uint64_t> KernelModel::retainProviderWake(uint64_t PDO,
                                                         uint64_t IRP) {
  auto *Request = requestForIRP(IRP);
  const auto *Provider = pnpDeviceForPDO(PDO);
  if (!Request || Request->Completed || Request->PnpDevice != PDO ||
      !Request->PowerOperation || !Request->ChildPower || !Provider ||
      Request->PowerOperation->Minor != DevicePowerRequest::WaitWake)
    return policyError("WAIT_WAKE requires its generated provider request");
  const bool Managed =
      Request->ChildPower->Origin == DriverRequestOrigin::FrameworkWaitWake;
  if (!Managed &&
      Request->ChildPower->Origin != DriverRequestOrigin::PoRequestPowerIrp)
    return policyError("WAIT_WAKE has an unsupported issuing owner");
  auto StartEpoch = deviceStartEpoch(PDO, true);
  if (!StartEpoch)
    return StartEpoch.takeError();
  ProviderWake Wake;
  Wake.IRP = IRP;
  Wake.StartEpoch = *StartEpoch;
  uint32_t Status = windows::StatusPending;
  auto CancelRequested = Memory.readInteger(IRP + windows::IRPCancelOffset, 1);
  if (!CancelRequested)
    return CancelRequested.takeError();
  if (Managed) {
    if (!Framework || ProviderWakeIRPs.contains(PDO))
      return policyError(
          "framework WAIT_WAKE requires one live provider owner");
    auto Epoch = Framework->powerPolicyEpoch(PDO);
    if (!Epoch)
      return Epoch.takeError();
    Wake.FrameworkEpoch = *Epoch;
  } else {
    if (Request->ChildPower->StartEpoch != *StartEpoch)
      return policyError("WAIT_WAKE send belongs to an earlier START epoch");
    auto State = Lifecycle.snapshot(PDO);
    if (!State)
      return State.takeError();
    if (*CancelRequested)
      Status = framework::RequestCancelled;
    else if (State->DevicePower != DevicePowerState::D0 ||
             State->DevicePowerOperation || State->SystemPowerOperation)
      Status = windows::StatusInvalidDeviceState;
    else if (ProviderWakeIRPs.contains(PDO))
      Status = windows::StatusDeviceBusy;
    else if (const auto *Usb = usbIdleConfig(PDO); Usb && !Usb->RemoteWake)
      Status = windows::StatusNotSupported;
    else if (!Provider->WakeCapabilities || (!Provider->WakeCapabilities->S0 &&
                                             !Provider->WakeCapabilities->Sx))
      Status = windows::StatusNotSupported;
    else if (!(Request->PowerOperation->State ==
                       uint32_t(SystemPowerState::Working)
                   ? Provider->WakeCapabilities->S0
                   : Provider->WakeCapabilities->Sx))
      Status = windows::StatusInvalidDeviceState;
  }
  auto Cancel = Memory.readInteger(IRP + windows::IRPCancelRoutineOffset, 8);
  if (!Cancel)
    return Cancel.takeError();
  if (*Cancel)
    return policyError("WAIT_WAKE reached the bus with a foreign cancel owner");
  auto &Observation = *Result.Requests[Request->ResultIndex].Power;
  if (Status != windows::StatusPending) {
    auto Plan = planIRPCompletion(IRP, Status);
    if (!Plan)
      return Plan.takeError();
    if (NextIRPCall == UINT64_MAX)
      return policyError("WAIT_WAKE completion identity exhausted");
    if (auto E = Memory.writeInteger(IRP + windows::IRPStatusOffset, Status, 4))
      return E;
    if (auto E = Memory.writeInteger(IRP + windows::IRPInformationOffset, 0, 8))
      return E;
    Request->IOStatusWritten.fill(true);
    Observation.BusReceivedAt100ns = Scheduler.now100ns();
    Observation.BusCompletedAt100ns = Scheduler.now100ns();
    Observation.BusStatus = Status;
    if (auto E = completeRequest(IRP, 0))
      return E;
    if (PendingWdmCall)
      IRPCalls.at(PendingWdmCall->Token.ID).ReturnValue = Status;
    return Status;
  }
  if (!Managed) {
    if (!Exports)
      return policyError(
          "native WAIT_WAKE requires a provider callback registry");
    auto Routine =
        Exports->insertProviderFunction(PDO, callback::CancelWaitWake);
    if (!Routine)
      return Routine.takeError();
    Wake.CancelRoutine = *Routine;
    if (auto E = Memory.writeInteger(IRP + windows::IRPCancelRoutineOffset,
                                     *Routine, 8))
      return E;
  }
  if (auto E = markRequestPending(IRP))
    return E;
  Observation.BusReceivedAt100ns = Scheduler.now100ns();
  ProviderWakeIRPs.emplace(PDO, std::move(Wake));
  return windows::StatusPending;
}

void KernelModel::configureFrameworkPowerPolicyHost() {
  KernelFramework::PowerPolicyHost Host;
  Host.ResolveUsbIdle = [this](uint64_t Device, uint32_t State) {
    return resolveFrameworkUsbIdle(Device, State);
  };
  Host.SubmitUsbIdle = [this](uint64_t Device, uint64_t Epoch) {
    return submitFrameworkUsbIdle(Device, Epoch);
  };
  Host.HasUsbIdle = [this](UsbIdleKey Key) { return hasFrameworkUsbIdle(Key); };
  Host.CancelUsbIdle =
      [this](UsbIdleKey Key,
             KernelFramework::PowerPolicyHost::RequestMode Mode) {
        return cancelFrameworkUsbIdle(Key, Mode);
      };
  Host.RequestUsbIdlePower =
      [this](uint64_t Device, UsbIdleKey Key, uint64_t Token,
             KernelFramework::PowerPolicyHost::RequestMode Mode) {
        return requestFrameworkUsbIdlePower(Device, Key, Token, Mode);
      };
  Host.AbortUsbIdlePower = [this](UsbIdleKey Key, uint64_t Token,
                                  uint32_t Status) {
    return abortFrameworkUsbIdlePower(Key, Token, Status);
  };
  Host.FinishUsbIdleCallback = [this](UsbIdleKey Key, uint64_t Token) {
    return finishFrameworkUsbIdleCallback(Key, Token);
  };
  Host.CompletePowerNotRequired =
      [this](uint64_t Device,
             KernelFramework::PowerPolicyHost::RequestMode Mode) {
        return completeFrameworkPowerNotRequired(Device, Mode);
      };
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
  Host.Children =
      [this](uint64_t PDO) -> llvm::Expected<std::vector<uint64_t>> {
    if (!isProviderDevice(PDO))
      return policyError("child lookup requires a live parent provider");
    std::vector<uint64_t> Children;
    for (const auto &[ID, Provider] : PnpDevices)
      if (Provider.ParentPDO == PDO && isProviderDevice(Provider.PDO))
        Children.push_back(Provider.PDO);
    return Children;
  };
  Host.CompleteWakes = [this](uint64_t Source,
                              llvm::ArrayRef<uint64_t> Devices) {
    return completeFrameworkWakes(Source, Devices);
  };
  Host.CancelWakes = [this](llvm::ArrayRef<uint64_t> Devices) {
    return cancelFrameworkWakes(Devices);
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
  if (ProviderWakeIRPs.contains(*PDO))
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
  if (!ProviderWakeIRPs.contains(*PDO))
    return llvm::Error::success();
  if (Triggered)
    return completeFrameworkWakes(Device, {Device});
  return cancelFrameworkWakes({Device});
}

llvm::Expected<uint64_t>
KernelModel::preflightFrameworkWake(uint64_t PDO, uint32_t Status) const {
  const auto Wake = ProviderWakeIRPs.find(PDO);
  if (Wake == ProviderWakeIRPs.end() || !Wake->second.FrameworkEpoch)
    return policyError("framework wake requires its retained WAIT_WAKE IRP");
  auto IRP = preflightProviderWake(PDO, Wake->second.IRP, Status);
  if (!IRP)
    return IRP.takeError();
  auto Plan = planIRPCompletion(*IRP, Status,
                                Status == framework::RequestCancelled
                                    ? std::optional<bool>{true}
                                    : std::nullopt);
  if (!Plan)
    return Plan.takeError();
  if (Plan->PC)
    return policyError(
        "framework wait/wake cannot own a WDM completion callback");
  return *IRP;
}

llvm::Expected<uint64_t>
KernelModel::preflightProviderWake(uint64_t PDO, uint64_t IRP,
                                   uint32_t Status) const {
  const auto Pending = ProviderWakeIRPs.find(PDO);
  if (Pending == ProviderWakeIRPs.end() || Pending->second.IRP != IRP)
    return policyError("wake completion requires its exact retained WAIT_WAKE");
  const auto &Wake = Pending->second;
  const auto *Request = requestForIRP(IRP);
  if (!Request || Request->Completed || !Request->PowerOperation ||
      Request->PowerOperation->Minor != DevicePowerRequest::WaitWake ||
      !Request->ChildPower || Request->PnpDevice != PDO ||
      Request->PowerTicket || Request->FrameworkTransitionAwaiting ||
      ProviderCompletions.contains(IRP))
    return policyError("wait/wake completion lost its retained IRP");
  const auto ExpectedOrigin = Wake.FrameworkEpoch
                                  ? DriverRequestOrigin::FrameworkWaitWake
                                  : DriverRequestOrigin::PoRequestPowerIrp;
  if (Request->ChildPower->Origin != ExpectedOrigin ||
      (Wake.FrameworkEpoch && Request->ChildPower->Callback))
    return policyError("wait/wake completion lost its issuing owner");
  if (CancelLock.Held || PendingWdmCall ||
      (Framework && Framework->hasPendingGuestCall()))
    return policyError(
        "wait/wake completion requires a free callback boundary");
  if (NextIRPCall == UINT64_MAX)
    return policyError("wait/wake completion identity exhausted");
  auto CancelRoutine = Memory.readInteger(IRP + windows::IRPCancelRoutineOffset,
                                          sizeof(uint64_t));
  if (!CancelRoutine)
    return CancelRoutine.takeError();
  const bool CancelCallback = Status == framework::RequestCancelled &&
                              CancelLock.Callback && CancelLock.IRP == IRP;
  if (*CancelRoutine != (CancelCallback ? 0 : Wake.CancelRoutine))
    return policyError(
        "wait/wake cancel routine lost its provider cancel owner");
  const auto &Observation = Result.Requests[Request->ResultIndex].Power;
  if (!Observation || !Observation->BusReceivedAt100ns ||
      Observation->BusCompletedAt100ns)
    return policyError("wait/wake completion lost its provider retention");
  if (Status == windows::StatusSuccess) {
    if (auto E = validateUsbRemoteWake(PDO))
      return E;
    auto Epoch = deviceStartEpoch(PDO, false);
    if (!Epoch)
      return Epoch.takeError();
    if (*Epoch != Wake.StartEpoch)
      return policyError("wait/wake belongs to an earlier START epoch");
    auto State = Lifecycle.snapshot(PDO);
    if (!State)
      return State.takeError();
    if (uint32_t(State->SystemPower) > Request->PowerOperation->State)
      return policyError("wake source cannot wake the current system state");
    const auto *Provider = pnpDeviceForPDO(PDO);
    if (!Provider || !Provider->WakeCapabilities ||
        !(State->SystemPower == SystemPowerState::Working
              ? Provider->WakeCapabilities->S0
              : Provider->WakeCapabilities->Sx))
      return policyError("provider cannot wake the current system state");
    if (Wake.FrameworkEpoch) {
      if (!Framework)
        return policyError("wake source lost its live framework provider");
      auto FrameworkEpoch = Framework->powerPolicyEpoch(PDO);
      if (!FrameworkEpoch)
        return FrameworkEpoch.takeError();
      if (*FrameworkEpoch != *Wake.FrameworkEpoch)
        return policyError("framework wait/wake belongs to an earlier START");
    }
  }
  auto Writable = Memory.canAccess(
      IRP, windows::IRPSize + Request->StackCount * windows::StackSize,
      Read | Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return policyError("wait/wake packet must remain writable kernel memory");
  auto Plan = planIRPCompletion(IRP, Status,
                                Status == framework::RequestCancelled
                                    ? std::optional<bool>{true}
                                    : std::nullopt);
  if (!Plan)
    return Plan.takeError();
  return IRP;
}

llvm::Error
KernelModel::completeProviderWake(uint64_t PDO, uint64_t IRP, uint32_t Status,
                                  std::optional<uint64_t> SourcePDO) {
  auto Validated = preflightProviderWake(PDO, IRP, Status);
  if (!Validated)
    return Validated.takeError();
  const auto *Source = SourcePDO ? pnpDeviceForPDO(*SourcePDO) : nullptr;
  if (SourcePDO && !Source)
    return policyError("wake completion lost its source provider");
  auto *Request = requestForIRP(IRP);
  if (auto E = Memory.writeInteger(IRP + windows::IRPCancelRoutineOffset, 0, 8))
    return E;
  if (auto E = Memory.writeInteger(IRP + windows::IRPStatusOffset, Status, 4))
    return E;
  if (auto E = Memory.writeInteger(IRP + windows::IRPInformationOffset, 0, 8))
    return E;
  Request->IOStatusWritten.fill(true);
  auto &Observation = *Result.Requests[Request->ResultIndex].Power;
  Observation.BusStatus = Status;
  Observation.BusCompletedAt100ns = Scheduler.now100ns();
  if (Source) {
    Observation.WakeSourceDeviceID = Result.PnpDevices[Source->ResultIndex].ID;
    Observation.WakeSourcePDO = *SourcePDO;
  }
  // Publish retirement before guest completions can rearm the same provider.
  ProviderWakeIRPs.erase(PDO);
  if (auto E = completeRequest(IRP, 0))
    return E;
  return tryFinalizePowerRequest(IRP);
}

llvm::Error
KernelModel::completeFrameworkWakes(uint64_t SourceDevice,
                                    llvm::ArrayRef<uint64_t> WakeDevices) {
  auto SourcePDO = pnpDeviceForRoute(SourceDevice);
  if (!SourcePDO)
    return SourcePDO.takeError();
  if (WakeDevices.empty() || WakeDevices.size() > UINT64_MAX - NextIRPCall)
    return policyError("wake batch requires bounded completion identities");
  std::vector<uint64_t> Providers;
  for (uint64_t Device : WakeDevices) {
    auto PDO = pnpDeviceForRoute(Device);
    if (!PDO)
      return PDO.takeError();
    if (std::find(Providers.begin(), Providers.end(), *PDO) != Providers.end())
      return policyError("wake batch cannot complete a source twice");
    const auto *Ancestor = pnpDeviceForPDO(*PDO);
    while (Ancestor && Ancestor->PDO != *SourcePDO)
      Ancestor = pnpDeviceForPDO(Ancestor->ParentPDO);
    if (!Ancestor)
      return policyError("propagated wake requires a configured descendant");
    auto IRP = preflightFrameworkWake(*PDO, windows::StatusSuccess);
    if (!IRP)
      return IRP.takeError();
    Providers.push_back(*PDO);
  }
  if (std::find(Providers.begin(), Providers.end(), *SourcePDO) ==
      Providers.end())
    return policyError("wake batch must include its original source");
  // No guest callback can run between validation and these completions. The
  // provider owns every writable packet, and each unwinds without a callback.
  for (uint64_t PDO : Providers)
    if (auto E = completeProviderWake(PDO, ProviderWakeIRPs.at(PDO).IRP,
                                      windows::StatusSuccess, *SourcePDO))
      return E;
  return llvm::Error::success();
}

llvm::Error
KernelModel::cancelFrameworkWakes(llvm::ArrayRef<uint64_t> WakeDevices) {
  std::set<uint64_t> Providers;
  for (uint64_t Device : WakeDevices) {
    auto PDO = pnpDeviceForRoute(Device);
    if (!PDO)
      return PDO.takeError();
    if (!ProviderWakeIRPs.contains(*PDO))
      continue;
    if (!Providers.insert(*PDO).second)
      return policyError("wake cancellation cannot retire a source twice");
    auto IRP = preflightFrameworkWake(*PDO, framework::RequestCancelled);
    if (!IRP)
      return IRP.takeError();
  }
  if (Providers.size() > UINT64_MAX - NextIRPCall)
    return policyError("wake cancellation identity exhausted");
  for (uint64_t PDO : Providers)
    if (auto E =
            completeProviderWake(PDO, ProviderWakeIRPs.at(PDO).IRP,
                                 framework::RequestCancelled, std::nullopt))
      return E;
  return llvm::Error::success();
}

} // namespace neverd::emulation
