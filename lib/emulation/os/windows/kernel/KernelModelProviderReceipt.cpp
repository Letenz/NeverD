//===- KernelModelProviderReceipt.cpp - Nested provider receipt effects --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
using namespace windows;

llvm::Error receiptError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "provider receipt: " + Message);
}
} // namespace

llvm::Expected<std::vector<UsbIdleCompletionPlan>>
KernelModel::planUsbIdleReceipt(uint64_t PDO, uint64_t IRP) const {
  const auto *Request = requestForIRP(IRP);
  if (!Request || Request->Completed || Request->PnpDevice != PDO)
    return receiptError("idle effects lost their original provider request");
  std::optional<UsbIdleCompletionCause> Cause;
  if (Request->PowerOperation &&
      Request->PowerOperation->Minor == DevicePowerRequest::Set) {
    const auto &Power = *Request->PowerOperation;
    if (Power.Type == DriverPowerType::System &&
        Power.State != uint32_t(SystemPowerState::Working))
      Cause = UsbIdleCompletionCause::SystemSleep;
    else if (Power.Type == DriverPowerType::Device) {
      if (Power.State == uint32_t(DevicePowerState::D0))
        Cause = UsbIdleCompletionCause::DeviceD0;
      else if (Power.State == uint32_t(DevicePowerState::D3))
        Cause = UsbIdleCompletionCause::DeviceD3;
    }
  } else if (Request->PnpOperation) {
    const auto Minor = Request->PnpOperation->Minor;
    if (Minor == DevicePnpRequest::Stop || Minor == DevicePnpRequest::Remove ||
        Minor == DevicePnpRequest::SurpriseRemoval)
      Cause = UsbIdleCompletionCause::Remove;
  }
  std::vector<UsbIdleCompletionPlan> Plans;
  const auto *Config = usbIdleConfig(PDO);
  if (!Cause || !Config)
    return Plans;
  std::vector<uint64_t> Members;
  if (Config->Role == DriverUsbIdleRole::CompositeParent) {
    for (const auto &[ID, Child] : PnpDevices) {
      const auto *ChildConfig = usbIdleConfig(Child.PDO);
      if (Child.ParentPDO == PDO && ChildConfig &&
          ChildConfig->Role == DriverUsbIdleRole::CompositeFunction)
        Members.push_back(Child.PDO);
    }
  } else {
    Members.push_back(PDO);
  }
  uint64_t Completions = 1;
  for (uint64_t Member : Members) {
    const auto *Submission = UsbIdle.submission(Member);
    if (!Submission)
      continue;
    const auto Key = Submission->Key;
    const auto *Provider = pnpDeviceForPDO(Member);
    const auto *IdleRequest = requestForIRP(Key.IRP);
    if (!Provider || !isProviderDevice(Member) ||
        Provider->StartEpoch != Key.StartEpoch || !IdleRequest ||
        IdleRequest->Completed || IdleRequest->PnpDevice != Member ||
        !Result.Requests[IdleRequest->ResultIndex].UsbIdle)
      return receiptError("idle effects lost a captured live START or packet");
    auto Claim = preflightUsbIdleCompletion(Key, *Cause);
    if (!Claim)
      return Claim.takeError();
    if (!Claim->DeferredUntilCallbackReturn)
      ++Completions;
    Plans.push_back(*Claim);
  }
  if (Completions > UINT64_MAX - NextIRPCall)
    return receiptError("completion identity exhausted");
  return Plans;
}

llvm::Expected<std::optional<uint64_t>>
KernelModel::continueProviderReceipt(uint64_t IRP) {
  auto Found = ProviderReceipts.find(IRP);
  if (Found == ProviderReceipts.end())
    return receiptError("nested completion lost its provider receipt");
  auto &Receipt = Found->second;
  while (Receipt.NextIdle < Receipt.Idle.size()) {
    const auto Plan = Receipt.Idle[Receipt.NextIdle];
    const auto *Current = UsbIdle.submission(Plan.Key.PDO);
    if (!Current || Current->Key != Plan.Key) {
      // An earlier member's completion may cancel another claimed member.
      // Accept only proof that the captured packet completed; never follow a
      // replacement registration, even when it has the same provider.
      const auto *Completed = requestForIRP(Plan.Key.IRP);
      if ((!Completed || !Completed->Completed) &&
          !FinalizedRequests.contains(Plan.Key.IRP))
        return receiptError(
            "captured idle claim disappeared before completion");
      ++Receipt.NextIdle;
      continue;
    }
    if (auto E = completeUsbIdle(Plan.Key, Plan.Cause))
      return E;
    ++Receipt.NextIdle;
    if (PendingWdmCall) {
      auto Call = IRPCalls.find(PendingWdmCall->Token.ID);
      if (PendingWdmCall->Token.Owner != GuestCallOwner::WDM ||
          Call == IRPCalls.end() || Call->second.IRP != Plan.Key.IRP ||
          Call->second.ProviderReceiptIRP)
        return receiptError("idle completion lost its exact return boundary");
      Call->second.ProviderReceiptIRP = IRP;
      return std::optional<uint64_t>{};
    }
  }
  const auto DispatchToken = Receipt.ScheduledDispatchToken;
  auto Status = completeProviderReceipt(Receipt.Device, IRP, Receipt.Deadline);
  if (!Status)
    return Status.takeError();
  ProviderReceipts.erase(Found);
  if (DispatchToken) {
    auto Returned = finishWdmGuestCallBody(*DispatchToken, *Status);
    if (!Returned)
      return Returned.takeError();
    if (!*Returned)
      return receiptError("scheduled dispatch lost its final return");
  }
  if (PendingWdmCall)
    return std::optional<uint64_t>{};
  return std::optional<uint64_t>{*Status};
}

llvm::Expected<uint64_t>
KernelModel::completeProviderReceipt(uint64_t PDO, uint64_t IRP,
                                     uint64_t Deadline) {
  auto *Request = requestForIRP(IRP);
  if (!Request || Request->Completed || Request->PnpDevice != PDO)
    return receiptError("acknowledgement lost its original provider request");
  const auto *Response =
      Request->PnpOperation     ? &Request->PnpOperation->BusCompletion
      : Request->PowerOperation ? &Request->PowerOperation->BusCompletion
                                : nullptr;
  if (!Response || !Response->Status)
    return receiptError("acknowledgement lost its configured response");
  auto &Observation = Result.Requests[Request->ResultIndex];
  const auto Received = Observation.Pnp ? Observation.Pnp->BusReceivedAt100ns
                        : Observation.Power
                            ? Observation.Power->BusReceivedAt100ns
                            : std::optional<uint64_t>{};
  if (!Received || ProviderCompletions.contains(IRP))
    return receiptError(
        "acknowledgement requires exactly one provider receipt");
  // Guest idle completion may have run since the receipt. Revalidate the
  // original packet without publishing another receipt or moving its deadline.
  auto Plan = planIRPCompletion(IRP, *Response->Status);
  if (!Plan)
    return Plan.takeError();
  if (NextIRPCall == UINT64_MAX || NextProviderSequence == UINT64_MAX)
    return receiptError("completion identity exhausted");
  if (Response->Delay100ns) {
    if (auto E = markRequestPending(IRP))
      return E;
    ProviderCompletions.emplace(IRP, ProviderCompletion{PDO, Deadline,
                                                        NextProviderSequence++,
                                                        *Response->Status});
    return StatusPending;
  }
  const uint32_t Status = *Response->Status;
  if (auto E = Memory.writeInteger(IRP + IRPStatusOffset, Status, 4))
    return E;
  if (auto E = Memory.writeInteger(IRP + IRPInformationOffset, 0, 8))
    return E;
  Request->IOStatusWritten.fill(true);
  auto Publish = [&](auto &Bus) {
    Bus.BusStatus = Status;
    Bus.BusCompletedAt100ns = Scheduler.now100ns();
  };
  if (Observation.Pnp)
    Publish(*Observation.Pnp);
  else
    Publish(*Observation.Power);
  if (Request->PowerOperation &&
      Request->PowerOperation->Type == DriverPowerType::Device &&
      Request->PowerOperation->Minor == DevicePowerRequest::Set &&
      !(Status & profile::NTStatusFailureMask))
    Devices.at(PDO).ReportedDevicePower =
        static_cast<DevicePowerState>(Request->PowerOperation->State);
  if (auto E = publishProviderHardware(*Request, Status))
    return E;
  if (auto E = completeRequest(IRP, 0))
    return E;
  // A generated child can retire during synchronous completion.
  const auto *Remaining = requestForIRP(IRP);
  if (Remaining && Remaining->FrameworkTransitionAwaiting)
    return StatusPending;
  if (PendingWdmCall)
    IRPCalls.at(PendingWdmCall->Token.ID).ReturnValue = Status;
  return Status;
}
} // namespace neverd::emulation
