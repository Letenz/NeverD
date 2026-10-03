//===- KernelModelUsbIdle.cpp - USB idle guest protocol bridge ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
using namespace windows;

llvm::Error usbError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "USB idle: " + Text);
}

namespace callback {
#define NEVERD_PROVIDER_CALLBACK(Name, Arity)                                  \
  constexpr llvm::StringLiteral Name = #Name;
#include "KernelProviderCallbacks.def"
#undef NEVERD_PROVIDER_CALLBACK
} // namespace callback

} // namespace

const DriverUsbIdleConfig *KernelModel::usbIdleConfig(uint64_t PDO) const {
  const auto *Provider = pnpDeviceForPDO(PDO);
  if (!Provider)
    return nullptr;
  const auto &ID = Result.PnpDevices[Provider->ResultIndex].ID;
  for (const auto &Device : ConfiguredPnpDevices)
    if (Device.ID == ID)
      return Device.UsbIdle ? &*Device.UsbIdle : nullptr;
  return nullptr;
}

llvm::Expected<UsbIdleSubmission>
KernelModel::planUsbIdleSubmission(uint64_t PDO, uint64_t IRP, uint64_t Info,
                                   uint64_t InputSize, uint64_t OutputSize,
                                   uint64_t Output) const {
  const auto *Config = usbIdleConfig(PDO);
  if (!Config || Config->Role == DriverUsbIdleRole::CompositeParent)
    return usbError("submission requires an explicit USB function provider");
  if (CurrentIRQL != scheduler::PassiveLevel)
    return usbError("submission requires PASSIVE_LEVEL");
  if (InputSize != usb_idle::CallbackInfoSize || OutputSize || Output ||
      Info < profile::UserProbeLimit)
    return usbError("submission requires kernel callback info and no output");
  auto Epoch = deviceStartEpoch(PDO, false);
  if (!Epoch)
    return Epoch.takeError();
  auto State = Lifecycle.snapshot(PDO);
  if (!State)
    return State.takeError();
  if (State->Pnp != DevicePnpState::Started ||
      State->DevicePower != DevicePowerState::D0 ||
      State->SystemPower != SystemPowerState::Working ||
      State->DevicePowerOperation || State->SystemPowerOperation)
    return usbError("submission requires stable S0/D0 in its live START");
  if (auto E = validateGuestAccessImpl(Info, InputSize, false, false))
    return E;
  auto PC =
      Memory.readInteger(Info + usb_idle::CallbackOffset, profile::PointerSize);
  auto Context =
      Memory.readInteger(Info + usb_idle::ContextOffset, profile::PointerSize);
  if (!PC || !Context)
    return llvm::joinErrors(PC.takeError(), Context.takeError());
  UsbIdleCallbackOwner Owner = UsbIdleCallbackOwner::Guest;
  const auto Native = FrameworkUsbIdleRequests.find(IRP);
  if (Native != FrameworkUsbIdleRequests.end()) {
    const auto *Entry = Exports ? Exports->lookup(*PC) : nullptr;
    if (Native->second.Info != Info ||
        Native->second.Key != UsbIdleKey{PDO, IRP, *Epoch} || *Context != IRP ||
        !Entry ||
        Entry->Kind != KernelExportRegistry::ExportKind::ProviderFunction ||
        Entry->Binding != PDO || Entry->Name != callback::FrameworkUsbIdle)
      return usbError(
          "native callback requires its exact registered provider entry");
    Owner = UsbIdleCallbackOwner::Framework;
  } else {
    auto Executable = Memory.canAccess(*PC, 1, Execute);
    if (!Executable)
      return Executable.takeError();
    if (!*PC || !*Executable)
      return usbError("idle callback requires executable guest code");
  }
  return UsbIdleSubmission{{PDO, IRP, *Epoch}, Info, *PC, *Context, Owner};
}

llvm::Expected<uint64_t> KernelModel::receiveUsbIdle(uint64_t PDO,
                                                     uint64_t IRP) {
  auto *Request = requestForIRP(IRP);
  if (!Request || Request->Kind != DriverRequestKind::InternalDeviceControl ||
      Request->PnpDevice != PDO ||
      (!DriverIRPs.contains(IRP) && !FrameworkUsbIdleRequests.contains(IRP)))
    return usbError("provider receipt requires its retained idle packet owner");
  auto Stack = currentRequestStack(IRP);
  if (!Stack)
    return Stack.takeError();
  const auto &Observation = Result.Requests[Request->ResultIndex];
  auto Code = Memory.readInteger(*Stack + StackIOControlOffset, 4);
  auto Info = Memory.readInteger(*Stack + StackType3InputOffset, 8);
  auto InputSize = Memory.readInteger(*Stack + StackInputLengthOffset, 4);
  auto OutputSize = Memory.readInteger(*Stack + StackParametersOffset, 4);
  if (!Code || !Info || !InputSize || !OutputSize)
    return llvm::joinErrors(
        llvm::joinErrors(Code.takeError(), Info.takeError()),
        llvm::joinErrors(InputSize.takeError(), OutputSize.takeError()));
  if (*Code != usb_idle::SubmitIdleNotification ||
      *Info != Request->UserInput || *InputSize != Request->InputSize ||
      *OutputSize != Request->OutputSize ||
      (Observation.UsbIdle && Observation.UsbIdle->BusReceivedAt100ns))
    return usbError("forwarded idle packet changed or was already received");
  auto Submission = planUsbIdleSubmission(PDO, IRP, *Info, *InputSize,
                                          *OutputSize, Request->UserBuffer);
  if (!Submission)
    return Submission.takeError();
  auto Cancel = Memory.readInteger(IRP + IRPCancelOffset, 1);
  auto CancelRoutine = Memory.readInteger(IRP + IRPCancelRoutineOffset, 8);
  if (!Cancel || !CancelRoutine)
    return llvm::joinErrors(Cancel.takeError(), CancelRoutine.takeError());
  if (*CancelRoutine)
    return usbError("provider cannot replace an installed cancel routine");
  const uint32_t Status = *Cancel ? framework::RequestCancelled
                          : UsbIdle.hasOutstanding(PDO) ? StatusDeviceBusy
                                                        : StatusPending;
  if (Status != StatusPending) {
    auto Plan = planIRPCompletion(IRP, Status);
    if (!Plan)
      return Plan.takeError();
    if (NextIRPCall == UINT64_MAX)
      return usbError("completion identity exhausted");
  } else {
    if (!Exports)
      return usbError("provider cancellation requires an export registry");
    if (auto E = UsbIdle.canSubmit(*Submission))
      return E;
  }
  std::optional<uint64_t> CancelPC;
  if (Status == StatusPending &&
      Submission->Owner == UsbIdleCallbackOwner::Guest) {
    auto Routine =
        Exports->insertProviderFunction(PDO, callback::CancelUsbIdle);
    if (!Routine)
      return Routine.takeError();
    CancelPC = *Routine;
  }
  auto &Facts = Result.Requests[Request->ResultIndex].UsbIdle.emplace();
  Facts.StartEpoch = Submission->Key.StartEpoch;
  Facts.BusReceivedAt100ns = Scheduler.now100ns();
  if (Status == StatusPending) {
    if (auto E = UsbIdle.submit(*Submission))
      return E;
    if (auto E = markRequestPending(IRP))
      return E;
    if (auto E = Memory.writeInteger(IRP + IRPCancelRoutineOffset,
                                     CancelPC.value_or(0), 8))
      return E;
    return StatusPending;
  }
  if (auto E = Memory.writeInteger(IRP + IRPStatusOffset, Status, 4))
    return E;
  if (auto E = Memory.writeInteger(IRP + IRPInformationOffset, 0, 8))
    return E;
  Request->IOStatusWritten.fill(true);
  if (auto E = completeRequest(IRP, 0))
    return E;
  if (PendingWdmCall)
    IRPCalls.at(PendingWdmCall->Token.ID).ReturnValue = Status;
  return Status;
}

llvm::Expected<std::vector<UsbIdleKey>>
KernelModel::captureUsbIdlePermission(uint64_t PDO) const {
  const auto *Config = usbIdleConfig(PDO);
  if (!Config)
    return usbError("permission requires an explicit USB coordinator");
  std::vector<uint64_t> Members;
  if (Config->Role == DriverUsbIdleRole::IndependentFunction) {
    Members.push_back(PDO);
  } else if (Config->Role == DriverUsbIdleRole::CompositeParent) {
    const auto *Parent = pnpDeviceForPDO(PDO);
    const auto &ParentID = Result.PnpDevices[Parent->ResultIndex].ID;
    for (const auto &Device : ConfiguredPnpDevices)
      if (Device.UsbIdle && Device.ParentID == ParentID &&
          Device.UsbIdle->Role == DriverUsbIdleRole::CompositeFunction) {
        const auto Found = PnpDevices.find(Device.ID);
        if (Found == PnpDevices.end() || !isProviderDevice(Found->second.PDO))
          return usbError("composite permission requires every live member");
        Members.push_back(Found->second.PDO);
      }
  } else {
    return usbError("composite function permission requires its coordinator");
  }
  auto Captured = UsbIdle.capturePermission(Members);
  if (!Captured)
    return Captured.takeError();
  if (auto E = validateUsbIdlePermission(*Captured))
    return E;
  return Captured;
}

llvm::Error
KernelModel::validateUsbIdlePermission(llvm::ArrayRef<UsbIdleKey> Keys) const {
  if (auto E = UsbIdle.canQueueCallbacks(Keys))
    return E;
  for (const auto &Key : Keys) {
    auto Epoch = deviceStartEpoch(Key.PDO, false);
    auto State = Lifecycle.snapshot(Key.PDO);
    if (!Epoch || !State)
      return llvm::joinErrors(Epoch.takeError(), State.takeError());
    if (*Epoch != Key.StartEpoch || State->Pnp != DevicePnpState::Started ||
        State->DevicePower != DevicePowerState::D0 ||
        State->SystemPower != SystemPowerState::Working ||
        State->DevicePowerOperation || State->SystemPowerOperation)
      return usbError("permission lost its stable S0/D0 START identity");
  }
  return llvm::Error::success();
}

llvm::Expected<std::vector<KernelScheduler::UsbIdleCallback>>
KernelModel::previewUsbIdleCallbacks(llvm::ArrayRef<UsbIdleKey> Keys) const {
  auto Plans = UsbIdle.previewCallbacks(Keys);
  if (!Plans)
    return Plans.takeError();
  std::vector<KernelScheduler::UsbIdleCallback> Calls;
  for (const auto &Plan : *Plans) {
    const bool Native = Plan.Owner == UsbIdleCallbackOwner::Framework;
    KernelScheduler::Callback Call{
        Plan.Key.IRP, Plan.Key.PDO, profile::WorkerThreadIdentity,
        Native ? 0 : Plan.PC,
        Native ? std::vector<uint64_t>{Plan.Key.PDO, Plan.Key.IRP, Plan.Token}
               : std::vector<uint64_t>{Plan.Context}};
    Calls.push_back({std::move(Call),
                     Native ? KernelScheduler::CallbackKind::FrameworkUsbIdle
                            : KernelScheduler::CallbackKind::UsbIdle});
  }
  return Calls;
}

llvm::Error
KernelModel::queueUsbIdlePermission(llvm::ArrayRef<UsbIdleKey> Keys) {
  if (auto E = validateUsbIdlePermission(Keys))
    return E;
  auto Callbacks = previewUsbIdleCallbacks(Keys);
  if (!Callbacks)
    return Callbacks.takeError();
  if (auto E = Scheduler.canEnqueueUsbIdleBatch(*Callbacks))
    return E;
  auto Plans = UsbIdle.queueCallbacks(Keys);
  if (!Plans)
    return Plans.takeError();
  for (size_t I = 0; I < Plans->size(); ++I) {
    const auto &Plan = (*Plans)[I];
    auto ID =
        Plan.Owner == UsbIdleCallbackOwner::Framework
            ? Scheduler.enqueueFrameworkUsbIdleCallback(
                  Plan.Key.IRP, Plan.Key.PDO, profile::WorkerThreadIdentity,
                  Plan.Token)
            : Scheduler.enqueueUsbIdleCallback(std::move((*Callbacks)[I].Call));
    if (!ID)
      return ID.takeError();
    ScheduledModelContinuations.emplace(
        *ID, GuestCallToken{GuestCallOwner::UsbIdle, Plan.Token});
  }
  return llvm::Error::success();
}

llvm::Error KernelModel::beginUsbIdleCallback(uint64_t Token) {
  const auto *Call = UsbIdle.callback(Token);
  if (!Call || CurrentIRQL != scheduler::PassiveLevel)
    return usbError("callback entry requires its passive registration");
  const auto Key = Call->Key;
  auto *Request = requestForIRP(Key.IRP);
  if (!Request || Request->Completed || Request->PnpDevice != Key.PDO ||
      !Result.Requests[Request->ResultIndex].UsbIdle)
    return usbError("callback lost its original request");
  auto Epoch = deviceStartEpoch(Key.PDO, false);
  auto State = Lifecycle.snapshot(Key.PDO);
  if (!Epoch || !State)
    return llvm::joinErrors(Epoch.takeError(), State.takeError());
  if (*Epoch != Key.StartEpoch || State->Pnp != DevicePnpState::Started ||
      State->DevicePower != DevicePowerState::D0 ||
      State->SystemPower != SystemPowerState::Working ||
      State->DevicePowerOperation || State->SystemPowerOperation)
    return usbError("callback entry lost its stable S0/D0 START identity");
  if (Call->Owner == UsbIdleCallbackOwner::Framework) {
    const auto Native = FrameworkUsbIdleRequests.find(Key.IRP);
    if (!Framework || Native == FrameworkUsbIdleRequests.end())
      return usbError("callback lost its native framework owner");
    if (auto E = Framework->canBeginUsbIdlePermission(
            Key, Native->second.PolicyEpoch, Token))
      return E;
  }
  if (auto E = UsbIdle.beginCallback(Token))
    return E;
  Result.Requests[Request->ResultIndex].UsbIdle->CallbackEnteredAt100ns =
      Scheduler.now100ns();
  return llvm::Error::success();
}

llvm::Expected<std::optional<uint64_t>>
KernelModel::finishUsbIdleCallback(uint64_t Token) {
  const auto *Call = UsbIdle.callback(Token);
  if (!Call || CurrentIRQL != scheduler::PassiveLevel)
    return usbError("callback return requires its passive registration");
  const auto Key = Call->Key;
  auto Complete = UsbIdle.finishCallback(Token);
  if (!Complete)
    return Complete.takeError();
  const auto *Request = requestForIRP(Key.IRP);
  Result.Requests[Request->ResultIndex].UsbIdle->CallbackReturnedAt100ns =
      Scheduler.now100ns();
  if (*Complete) {
    if (auto E = completeUsbIdle(Key, (**Complete).Cause))
      return E;
    if (PendingWdmCall) {
      if (Scheduler.active()) {
        auto Found = ScheduledModelContinuations.find(Scheduler.active()->ID);
        if (Found != ScheduledModelContinuations.end() &&
            Found->second.Owner == GuestCallOwner::UsbIdle &&
            Found->second.ID == Token)
          Found->second = PendingWdmCall->Token;
      }
      return std::optional<uint64_t>{};
    }
  }
  return std::optional<uint64_t>{0};
}

llvm::Expected<UsbIdleCompletionPlan> KernelModel::preflightUsbIdleCompletion(
    UsbIdleKey Key, UsbIdleCompletionCause Cause,
    std::optional<bool> CancelOverride) const {
  auto Claim = UsbIdle.planCompletion(Key, Cause);
  if (!Claim)
    return Claim.takeError();
  const auto *Request = requestForIRP(Key.IRP);
  if (!Request || Request->Completed || Request->PnpDevice != Key.PDO ||
      !Result.Requests[Request->ResultIndex].UsbIdle)
    return usbError("completion lost its caller-owned request");
  if (Claim->DeferredUntilCallbackReturn)
    return Claim;
  if (CancelLock.Held || PendingWdmCall ||
      (Framework && Framework->hasPendingGuestCall()) ||
      ProviderCompletions.contains(Key.IRP) ||
      Request->FrameworkTransitionAwaiting)
    return usbError("completion still has another active owner");
  auto Plan = planIRPCompletion(
      Key.IRP, KernelUsbIdle::completionStatus(Claim->Cause), CancelOverride);
  if (!Plan)
    return Plan.takeError();
  if (NextIRPCall == UINT64_MAX)
    return usbError("completion identity exhausted");
  for (const auto &[ID, Token] : ScheduledModelContinuations) {
    if (Token.Owner != GuestCallOwner::UsbIdle)
      continue;
    const auto *Call = UsbIdle.callback(Token.ID);
    if (Call && Call->Key == Key)
      if (auto E = Scheduler.canWithdrawUsbIdleCallback(ID))
        return E;
  }
  return Claim;
}

llvm::Error KernelModel::completeUsbIdle(UsbIdleKey Key,
                                         UsbIdleCompletionCause Cause) {
  auto Claim = preflightUsbIdleCompletion(Key, Cause);
  if (!Claim)
    return Claim.takeError();
  auto *Request = requestForIRP(Key.IRP);
  const uint32_t Status = KernelUsbIdle::completionStatus(Claim->Cause);
  std::optional<std::pair<uint64_t, uint64_t>> Withdraw;
  if (!Claim->DeferredUntilCallbackReturn) {
    for (const auto &[ID, Token] : ScheduledModelContinuations) {
      if (Token.Owner != GuestCallOwner::UsbIdle)
        continue;
      const auto *Call = UsbIdle.callback(Token.ID);
      if (Call && Call->Key == Key) {
        Withdraw = std::pair{ID, Token.ID};
        break;
      }
    }
  }
  if (auto E = UsbIdle.claimCompletion(*Claim))
    return E;
  if (Withdraw) {
    if (auto E = Scheduler.withdrawUsbIdleCallback(Withdraw->first))
      return E;
    if (auto E = UsbIdle.withdrawCallback(Withdraw->second))
      return E;
    ScheduledModelContinuations.erase(Withdraw->first);
  }
  auto &Facts = *Result.Requests[Request->ResultIndex].UsbIdle;
  if (!Facts.CompletionCause) {
    Facts.CompletionCause = Claim->Cause;
    Facts.CompletionClaimedAt100ns = Scheduler.now100ns();
  }
  if (Claim->DeferredUntilCallbackReturn)
    return llvm::Error::success();
  if (auto E = Memory.writeInteger(Key.IRP + IRPCancelRoutineOffset, 0, 8))
    return E;
  if (auto E = Memory.writeInteger(Key.IRP + IRPStatusOffset, Status, 4))
    return E;
  if (auto E = Memory.writeInteger(Key.IRP + IRPInformationOffset, 0, 8))
    return E;
  Request->IOStatusWritten.fill(true);
  if (auto E = UsbIdle.retireCompletion(Key))
    return E;
  if (auto E = completeRequest(Key.IRP, 0))
    return E;
  return tryFinalizeFrameworkUsbIdle(Key.IRP);
}

llvm::Expected<uint64_t>
KernelModel::cancelUsbIdle(const KernelExportRegistry::Export &Export,
                           llvm::ArrayRef<uint64_t> Arguments) {
  const auto *Submission = UsbIdle.submission(Arguments[0]);
  const auto Call = IRPCalls.find(CurrentGuestCall.ID);
  if (!Submission || Submission->Key.IRP != Arguments[1] ||
      Export.Binding != Arguments[0] ||
      CurrentGuestCall.Owner != GuestCallOwner::WDM || Call == IRPCalls.end() ||
      Call->second.Kind != IRPCallKind::Cancel ||
      !Call->second.AwaitingCallback || Call->second.IRP != Arguments[1] ||
      !CancelLock.Callback || !CancelLock.Held ||
      CancelLock.IRP != Arguments[1] || CancelLock.Owner != CurrentExecution)
    return usbError("cancel callback requires its active provider owner");
  const auto Key = Submission->Key;
  auto Plan = UsbIdle.planCompletion(Key, UsbIdleCompletionCause::Cancel);
  if (!Plan)
    return Plan.takeError();
  if (auto E = releaseCancelSpinLock(CancelLock.OldIRQL))
    return E;
  if (auto E = completeUsbIdle(Key, UsbIdleCompletionCause::Cancel))
    return E;
  return 0;
}

llvm::Error KernelModel::observeUsbIdlePowerCompletion(uint64_t IRP,
                                                       uint32_t Status) {
  const auto Key = UsbIdle.keyForDevicePower(IRP);
  if (!Key)
    return llvm::Error::success();
  if (auto E = UsbIdle.completedDevicePower(IRP, Status))
    return E;
  const auto *Idle = requestForIRP(Key->IRP);
  auto &Facts = *Result.Requests[Idle->ResultIndex].UsbIdle;
  Facts.D2Status = Status;
  Facts.D2CompletedAt100ns = Scheduler.now100ns();
  if (FrameworkUsbIdleRequests.contains(Key->IRP)) {
    const auto *Call = UsbIdle.callbackForIRP(Key->IRP);
    if (!Call || Call->Owner != UsbIdleCallbackOwner::Framework)
      return usbError("D2 completion lost its native callback");
    auto Returned = finishUsbIdleCallback(Call->Token);
    if (!Returned)
      return Returned.takeError();
    if (!*Returned)
      return usbError("native callback return unexpectedly invoked guest code");
  }
  return llvm::Error::success();
}

llvm::Error KernelModel::validateUsbRemoteWake(uint64_t PDO) const {
  const auto *Config = usbIdleConfig(PDO);
  if (!Config)
    return llvm::Error::success();
  auto State = Lifecycle.snapshot(PDO);
  if (!State)
    return State.takeError();
  if (!Config->RemoteWake || State->DevicePower != DevicePowerState::D2)
    return usbError(
        "remote wake requires explicit USB capability and actual D2");
  return llvm::Error::success();
}
} // namespace neverd::emulation
