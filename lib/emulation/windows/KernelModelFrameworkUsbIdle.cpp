//===- KernelModelFrameworkUsbIdle.cpp - Framework USB packet ownership ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "llvm/Support/MathExtras.h"

namespace neverd::emulation {
namespace {
using namespace windows;
using RequestMode = KernelFramework::PowerPolicyHost::RequestMode;

llvm::Error usbError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "framework USB idle: " + Message);
}

namespace callback {
#define NEVERD_PROVIDER_CALLBACK(Name, Arity)                                  \
  constexpr llvm::StringLiteral Name = #Name;
#include "KernelProviderCallbacks.def"
#undef NEVERD_PROVIDER_CALLBACK
} // namespace callback
} // namespace

llvm::Expected<KernelFramework::PowerPolicyHost::UsbIdleSettings>
KernelModel::resolveFrameworkUsbIdle(uint64_t Device,
                                     uint32_t RequestedState) const {
  auto PDO = pnpDeviceForRoute(Device);
  if (!PDO)
    return PDO.takeError();
  const auto *Config = usbIdleConfig(*PDO);
  const auto *Provider = pnpDeviceForPDO(*PDO);
  if (!Framework || !FrameworkDevices.contains(Device) || !Config ||
      !Provider || Config->Role == DriverUsbIdleRole::CompositeParent)
    return usbError(
        "policy requires an explicit USB function and framework device");
  DevicePowerState Target = DevicePowerState(RequestedState);
  if (RequestedState == power_policy::DevicePowerMaximum) {
    if (!Config->DeviceWake)
      return usbError("PowerDeviceMaximum requires explicit USB DeviceWake");
    Target = *Config->DeviceWake;
  }
  if (Target != DevicePowerState::D2)
    return usbError("selective suspend requires the supported D2 state");
  const bool CanWake = Config->RemoteWake && Provider->WakeCapabilities &&
                       Provider->WakeCapabilities->S0;
  return KernelFramework::PowerPolicyHost::UsbIdleSettings{Target, CanWake};
}

llvm::Expected<UsbIdleKey>
KernelModel::submitFrameworkUsbIdle(uint64_t Device, uint64_t PolicyEpoch) {
  auto Settings =
      resolveFrameworkUsbIdle(Device, uint32_t(DevicePowerState::D2));
  if (!Settings)
    return Settings.takeError();
  auto PDO = pnpDeviceForRoute(Device);
  if (!PDO)
    return PDO.takeError();
  auto Epoch = deviceStartEpoch(*PDO, false);
  auto FrameworkEpoch = Framework->powerPolicyEpoch(*PDO);
  auto State = Lifecycle.snapshot(*PDO);
  if (!Epoch || !FrameworkEpoch || !State)
    return llvm::joinErrors(
        Epoch.takeError(),
        llvm::joinErrors(FrameworkEpoch.takeError(), State.takeError()));
  if (*FrameworkEpoch != PolicyEpoch ||
      CurrentIRQL != scheduler::PassiveLevel ||
      State->Pnp != DevicePnpState::Started ||
      State->DevicePower != DevicePowerState::D0 ||
      State->SystemPower != SystemPowerState::Working ||
      State->DevicePowerOperation || State->SystemPowerOperation ||
      UsbIdle.hasOutstanding(*PDO) || hasPendingModelGuestCall())
    return usbError("submission requires its idle, stable S0/D0 START");
  auto Route = deviceStack(Device);
  if (!Route)
    return Route.takeError();
  if (Route->size() != 2 || Route->back() != *PDO || !Exports)
    return usbError("submission requires the framework's direct FDO/PDO route");
  for (uint64_t Member : *Route)
    if (Devices.at(Member).DeletePending ||
        Devices.at(Member).InternalReferences == UINT64_MAX)
      return usbError("submission route cannot retain its device");
  const uint64_t PacketSize = IRPSize + Route->size() * StackSize;
  const uint64_t Packet = llvm::alignTo(NextAllocation, PoolAlignment);
  if (Packet > AllocationEnd || PacketSize > AllocationEnd - Packet)
    return usbError("idle packet exceeds the kernel arena");
  const uint64_t Info = llvm::alignTo(Packet + PacketSize, PoolAlignment);
  if (Info > AllocationEnd || usb_idle::CallbackInfoSize > AllocationEnd - Info)
    return usbError("callback info exceeds the kernel arena");
  auto Writable = Memory.canAccess(
      Packet, Info + usb_idle::CallbackInfoSize - Packet, Read | Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return usbError("packet and callback info require writable kernel storage");
  auto PC = Exports->insertProviderFunction(*PDO, callback::FrameworkUsbIdle);
  if (!PC)
    return PC.takeError();
  UsbIdleSubmission Submission{{*PDO, Packet, *Epoch},
                               Info,
                               *PC,
                               Packet,
                               UsbIdleCallbackOwner::Framework};
  if (auto E = UsbIdle.canSubmit(Submission))
    return E;
  auto AllocatedPacket = allocate(PacketSize, PoolAlignment);
  if (!AllocatedPacket)
    return AllocatedPacket.takeError();
  auto AllocatedInfo = allocate(usb_idle::CallbackInfoSize, PoolAlignment);
  if (!AllocatedInfo)
    return AllocatedInfo.takeError();
  if (*AllocatedPacket != Packet || *AllocatedInfo != Info)
    return usbError("validated packet allocation changed before commitment");
  if (auto E = Memory.writeInteger(Info + usb_idle::CallbackOffset, *PC,
                                   profile::PointerSize))
    return E;
  if (auto E = Memory.writeInteger(Info + usb_idle::ContextOffset, Packet,
                                   profile::PointerSize))
    return E;
  DriverRequest Input;
  Input.Kind = DriverRequestKind::InternalDeviceControl;
  Input.DeviceID = Result.PnpDevices[pnpDeviceForPDO(*PDO)->ResultIndex].ID;
  Input.ControlCode = usb_idle::SubmitIdleNotification;
  Input.Input.resize(usb_idle::CallbackInfoSize);
  ActiveRequest Record{Input.Kind, Result.Requests.size()};
  Record.IRP = Packet;
  Record.Device = Device;
  Record.PnpDevice = *PDO;
  Record.StackCount = Route->size();
  Record.Stack = Packet + IRPSize + (Record.StackCount - 1) * StackSize;
  Record.DeviceRoute = std::move(*Route);
  Record.UnwoundPending.resize(Record.StackCount);
  Record.Neither = true;
  Record.UserInput = Info;
  Record.InputSize = usb_idle::CallbackInfoSize;
  auto &Request = Requests.emplace(Packet, std::move(Record)).first->second;
  for (uint64_t Member : Request.DeviceRoute)
    if (auto E = retainDevice(Member))
      return E;
  if (auto E = initializeRequestPacket(Request, Input))
    return E;
  DriverRequestResult Observation;
  Observation.Kind = Input.Kind;
  Observation.Origin = DriverRequestOrigin::FrameworkUsbIdle;
  Observation.DeviceID = Input.DeviceID;
  Observation.IRP = Packet;
  Observation.ControlCode = Input.ControlCode;
  Result.Requests.push_back(std::move(Observation));
  FrameworkUsbIdleRequests.emplace(
      Packet,
      FrameworkUsbIdleRequest{Device, PolicyEpoch, Info, Submission.Key});
  auto Status = forwardFrameworkTransitionRequest(Packet);
  if (!Status)
    return Status.takeError();
  if (*Status != StatusPending)
    return usbError("provider did not retain the admitted framework packet");
  if (auto E = recordDispatchReturn(Packet, *Status))
    return E;
  return Submission.Key;
}

llvm::Expected<bool> KernelModel::hasFrameworkUsbIdle(UsbIdleKey Key) const {
  const auto Found = FrameworkUsbIdleRequests.find(Key.IRP);
  if (Found == FrameworkUsbIdleRequests.end()) {
    if (FinalizedRequests.contains(Key.IRP))
      return false;
    return usbError("registration has no native packet identity");
  }
  if (Found->second.Key != Key)
    return usbError("registration belongs to another START or provider");
  const auto *Submission = UsbIdle.submissionForIRP(Key.IRP);
  if (!Submission || Submission->Key != Key ||
      Submission->Owner != UsbIdleCallbackOwner::Framework)
    return usbError("native packet lost its retained registration");
  return true;
}

llvm::Error KernelModel::cancelFrameworkUsbIdle(UsbIdleKey Key,
                                                RequestMode Mode) {
  auto Live = hasFrameworkUsbIdle(Key);
  if (!Live)
    return Live.takeError();
  if (!*Live)
    return llvm::Error::success();
  auto Plan = preflightUsbIdleCompletion(Key, UsbIdleCompletionCause::Cancel);
  if (!Plan)
    return Plan.takeError();
  if (Mode == RequestMode::Validate)
    return llvm::Error::success();
  const auto *Entered = Plan->DeferredUntilCallbackReturn
                            ? UsbIdle.callbackForIRP(Key.IRP)
                            : nullptr;
  if (Plan->DeferredUntilCallbackReturn && !Entered)
    return usbError("entered registration lost its native callback token");
  const uint64_t Token = Entered ? Entered->Token : 0;
  auto *Request = requestForIRP(Key.IRP);
  if (auto E = Memory.writeInteger(Key.IRP + IRPCancelOffset, 1, 1))
    return E;
  Request->CancelRequested = true;
  auto &Observation = Result.Requests[Request->ResultIndex];
  if (!Observation.CancelRequestedAt100ns)
    Observation.CancelRequestedAt100ns = Scheduler.now100ns();
  if (auto E = completeUsbIdle(Key, UsbIdleCompletionCause::Cancel))
    return E;
  if (Token)
    return UsbIdle.cancelFrameworkCallback(Token);
  return llvm::Error::success();
}

llvm::Error KernelModel::requestFrameworkUsbIdlePower(uint64_t Device,
                                                      UsbIdleKey Key,
                                                      uint64_t Token,
                                                      RequestMode Mode) {
  const auto Native = FrameworkUsbIdleRequests.find(Key.IRP);
  const auto *Call = UsbIdle.callback(Token);
  if (Native == FrameworkUsbIdleRequests.end() || Native->second.Key != Key ||
      Native->second.Device != Device || !Call || Call->Key != Key ||
      Call->Owner != UsbIdleCallbackOwner::Framework)
    return usbError("D2 request lost its exact framework callback");
  const auto *Provider = pnpDeviceForPDO(Key.PDO);
  if (!Provider ||
      Provider->RequestedPowerIndex >= Provider->RequestedDevicePower.size())
    return usbError("D2 requires an explicit power response FIFO entry");
  const uint32_t Index = Provider->RequestedPowerIndex;
  DriverRequest Input;
  Input.Kind = DriverRequestKind::Power;
  Input.DeviceID = Result.PnpDevices[Provider->ResultIndex].ID;
  Input.Power = Provider->RequestedDevicePower[Index];
  if (Input.Power->Type != DriverPowerType::Device ||
      Input.Power->Minor != DevicePowerRequest::Set ||
      Input.Power->State != uint32_t(DevicePowerState::D2) ||
      Input.Power->Action != DriverPowerAction::None)
    return usbError("D2 does not match the next power response");
  RequestedPower Child;
  Child.RequestDevice = Device;
  Child.ResponseIndex = Index;
  Child.Origin = DriverRequestOrigin::FrameworkPowerPolicy;
  auto Plan = planPowerRequest(Input, Result.Requests.size(), Child,
                               PowerRequestDelivery::Inline);
  if (!Plan)
    return Plan.takeError();
  auto Space = canAllocatePowerRequest(*Plan, false);
  if (!Space)
    return Space.takeError();
  if (!*Space) {
    if (Mode == RequestMode::Validate)
      return usbError("D2 packet exceeds the available kernel arena");
    if (auto E = UsbIdle.failFrameworkCallback(
            Token, KernelUsbIdle::FrameworkCallbackFailure::PowerAllocation,
            StatusInsufficientResources))
      return E;
    if (auto E = cancelFrameworkUsbIdle(Key, RequestMode::Issue))
      return E;
    return Framework->finishUsbIdlePowerAdmissionFailure(
        Key, Token, StatusInsufficientResources);
  }
  if (Mode == RequestMode::Validate)
    return llvm::Error::success();
  if (auto E = UsbIdle.canIssueDevicePower(Token, DevicePowerRequest::Set,
                                           DevicePowerState::D2))
    return E;
  auto Invocation = commitPowerRequest(Input, Result.Requests.size(), Child,
                                       std::move(*Plan));
  if (!Invocation)
    return Invocation.takeError();
  ++pnpDeviceForPDO(Key.PDO)->RequestedPowerIndex;
  if (auto E = UsbIdle.issuedDevicePower(Token, Invocation->IRP))
    return E;
  const auto *Idle = requestForIRP(Key.IRP);
  Result.Requests[Idle->ResultIndex].UsbIdle->D2IRP = Invocation->IRP;
  auto Status = dispatchPreparedPowerRequest(*Invocation);
  if (!Status)
    return Status.takeError();
  if (auto E = recordDispatchReturn(Invocation->IRP, *Status))
    return E;
  return tryFinalizePowerRequest(Invocation->IRP);
}

llvm::Error KernelModel::abortFrameworkUsbIdlePower(UsbIdleKey Key,
                                                    uint64_t Token,
                                                    uint32_t Status) {
  const auto Native = FrameworkUsbIdleRequests.find(Key.IRP);
  const auto *Call = UsbIdle.callback(Token);
  if (Native == FrameworkUsbIdleRequests.end() || Native->second.Key != Key ||
      !Call || Call->Key != Key ||
      Call->Owner != UsbIdleCallbackOwner::Framework)
    return usbError("arm failure lost its exact framework callback");
  if (auto E = UsbIdle.failFrameworkCallback(
          Token, KernelUsbIdle::FrameworkCallbackFailure::ArmWake, Status))
    return E;
  if (auto E = cancelFrameworkUsbIdle(Key, RequestMode::Issue))
    return E;
  return finishFrameworkUsbIdleCallback(Key, Token);
}

llvm::Error KernelModel::finishFrameworkUsbIdleCallback(UsbIdleKey Key,
                                                        uint64_t Token) {
  const auto Native = FrameworkUsbIdleRequests.find(Key.IRP);
  const auto *Call = UsbIdle.callback(Token);
  if (Native == FrameworkUsbIdleRequests.end() || Native->second.Key != Key ||
      !Call || Call->Key != Key ||
      Call->Owner != UsbIdleCallbackOwner::Framework)
    return usbError("callback return lost its exact framework packet");
  auto Returned = finishUsbIdleCallback(Token);
  if (!Returned)
    return Returned.takeError();
  if (!*Returned)
    return usbError(
        "native callback retirement unexpectedly invoked guest code");
  return llvm::Error::success();
}

llvm::Error KernelModel::tryFinalizeFrameworkUsbIdle(uint64_t IRP) {
  const auto Native = FrameworkUsbIdleRequests.find(IRP);
  const auto *Request = requestForIRP(IRP);
  if (Native == FrameworkUsbIdleRequests.end() || !Request ||
      !Request->Completed || !Request->DispatchReturned)
    return llvm::Error::success();
  for (const auto &[ID, Call] : IRPCalls)
    if (Call.IRP == IRP)
      return llvm::Error::success();
  if (auto E = Framework->retireUsbIdleRegistration(Native->second.Key))
    return E;
  return finalizeRequest(IRP);
}

llvm::Expected<std::optional<KernelScheduler::Invocation>>
KernelModel::dispatchScheduledFrameworkUsbIdle(
    const KernelScheduler::Invocation &Call) {
  auto Continuation = ScheduledModelContinuations.find(Call.ID);
  const auto Native = FrameworkUsbIdleRequests.find(Call.Object);
  if (Call.Kind != KernelScheduler::CallbackKind::FrameworkUsbIdle || Call.PC ||
      Call.IRQL != scheduler::PassiveLevel || Call.Arguments.size() != 3 ||
      Call.Arguments[0] != Call.Owner || Call.Arguments[1] != Call.Object ||
      Continuation == ScheduledModelContinuations.end() ||
      Continuation->second.Owner != GuestCallOwner::UsbIdle ||
      Continuation->second.ID != Call.Arguments[2] ||
      Native == FrameworkUsbIdleRequests.end() ||
      Native->second.Key.PDO != Call.Owner)
    return usbError("scheduled permission lost its native packet identity");
  const uint64_t Token = Continuation->second.ID;
  const auto Key = Native->second.Key;
  const uint64_t Epoch = Native->second.PolicyEpoch;
  CurrentIRQL = scheduler::PassiveLevel;
  if (auto E = beginUsbIdleCallback(Token))
    return E;
  if (auto E = Framework->beginUsbIdlePermission(Key, Epoch, Token))
    return E;
  if (auto Guest = takeGuestCall()) {
    if (Guest->Token.Owner != GuestCallOwner::Framework)
      return usbError("permission produced a foreign guest continuation");
    auto Next = Scheduler.beginFrameworkUsbIdleCallback(
        Call.ID, {Call.Object, Call.Owner, Call.Thread, Guest->PC,
                  std::move(Guest->Arguments), Guest->SynchronizationObject});
    if (!Next)
      return Next.takeError();
    Continuation->second = Guest->Token;
    return std::optional<KernelScheduler::Invocation>{std::move(*Next)};
  }
  ScheduledModelContinuations.erase(Continuation);
  if (auto E = finishScheduled(Call.ID))
    return E;
  return std::optional<KernelScheduler::Invocation>{};
}
} // namespace neverd::emulation
