//===- KernelModelPowerCompletion.cpp - Requested power IRP lifetime -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Real PoRequestPowerIrp dispatch and its terminal void callback retain
/// independent request identities and never borrow a scenario request result.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
using namespace windows;

llvm::Error powerError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "requested power IRP: " + Message);
}
} // namespace

llvm::Expected<uint64_t>
KernelModel::requestPowerIrp(llvm::ArrayRef<uint64_t> Arguments) {
  if (Arguments.size() != 6)
    return powerError("PoRequestPowerIrp requires six arguments");
  if (CurrentIRQL > scheduler::DispatchLevel)
    return powerError("PoRequestPowerIrp requires IRQL <= DISPATCH_LEVEL");
  const auto Minor = static_cast<DevicePowerRequest>(uint8_t(Arguments[1]));
  const bool WaitWake = Minor == DevicePowerRequest::WaitWake;
  if (!WaitWake && Minor != DevicePowerRequest::Set &&
      Minor != DevicePowerRequest::Query)
    return StatusInvalidParameter2;
  if (WaitWake && CurrentIRQL != scheduler::PassiveLevel)
    return powerError("WAIT_WAKE issuance requires PASSIVE_LEVEL");
  const auto Delivery = CurrentIRQL == scheduler::PassiveLevel
                            ? PowerRequestDelivery::Inline
                            : PowerRequestDelivery::Queued;
  if (Arguments[5]) {
    if (!WaitWake)
      return powerError("Query/Set power requires a null output IRP pointer");
    if (auto E = validateGuestAccess(Arguments[5], profile::PointerSize, true))
      return E;
    auto Writable = Memory.canAccess(Arguments[5], profile::PointerSize, Write);
    if (!Writable)
      return Writable.takeError();
    if (!*Writable)
      return powerError("WAIT_WAKE output IRP pointer must be writable");
  }
  if (PendingWdmCall || (Framework && Framework->hasPendingGuestCall()))
    return powerError("cannot replace a pending guest callback");
  if (NextIRPCall == UINT64_MAX)
    return powerError("continuation identity exhausted");
  const uint64_t Device = Arguments[0];
  auto PDO = pnpDeviceForRoute(Device);
  if (!PDO)
    return PDO.takeError();
  auto *Provider = pnpDeviceForPDO(*PDO);
  if (!*PDO || !Provider || !Devices.count(Device) ||
      Devices.at(Device).DeletePending)
    return powerError("request requires a live configured PDO or FDO");
  const uint64_t UsbToken = CurrentGuestCall.Owner == GuestCallOwner::UsbIdle
                                ? CurrentGuestCall.ID
                                : 0;
  if (UsbToken && !WaitWake) {
    const auto *Idle = UsbIdle.callback(UsbToken);
    if (!Idle || Idle->Key.PDO != *PDO)
      return powerError("USB idle callback must power its own provider");
    if (auto E = UsbIdle.canIssueDevicePower(UsbToken, Minor,
                                             DevicePowerState(Arguments[2])))
      return E;
  }
  const uint32_t Index = Provider->RequestedPowerIndex;
  DriverPowerOperation Operation;
  if (WaitWake) {
    Operation.Minor = Minor;
    Operation.Type = DriverPowerType::System;
    Operation.State = uint32_t(Arguments[2]);
  } else {
    if (Index >= Provider->RequestedDevicePower.size())
      return powerError("requested_device_power response FIFO is exhausted");
    Operation = Provider->RequestedDevicePower[Index];
    if (Operation.Type != DriverPowerType::Device || Operation.Minor != Minor ||
        Operation.State != uint32_t(Arguments[2]))
      return powerError("PoRequestPowerIrp does not match the next explicit "
                        "requested_device_power response");
  }
  DriverRequest Input;
  Input.Kind = DriverRequestKind::Power;
  Input.DeviceID = Result.PnpDevices[Provider->ResultIndex].ID;
  Input.Power = Operation;
  RequestedPower Child;
  Child.RequestDevice = Device;
  Child.Callback = Arguments[3];
  Child.Context = Arguments[4];
  Child.ResponseIndex = Index;
  auto Plan = planPowerRequest(Input, Result.Requests.size(), Child, Delivery);
  if (!Plan)
    return Plan.takeError();
  auto Space = canAllocatePowerRequest(*Plan, Child.Callback != 0);
  if (!Space)
    return Space.takeError();
  if (!*Space) {
    if (UsbToken && !WaitWake)
      if (auto E = UsbIdle.failedDevicePowerAdmission(
              UsbToken, StatusInsufficientResources))
        return E;
    return StatusInsufficientResources;
  }
  if (Delivery == PowerRequestDelivery::Queued)
    if (auto E = Scheduler.canEnqueueWDMDispatch())
      return E;
  auto Invocation = commitPowerRequest(Input, Result.Requests.size(),
                                       std::move(Child), std::move(*Plan));
  if (!Invocation)
    return Invocation.takeError();
  // The real packet and its lifecycle ticket reserve the captured route before
  // an elevated caller returns, without changing that caller's execution level.
  const uint64_t IRP = Invocation->IRP;
  if (UsbToken && !WaitWake) {
    const auto Key = UsbIdle.callback(UsbToken)->Key;
    if (auto E = UsbIdle.issuedDevicePower(UsbToken, IRP))
      return E;
    const auto *Idle = requestForIRP(Key.IRP);
    Result.Requests[Idle->ResultIndex].UsbIdle->D2IRP = IRP;
  }
  if (Arguments[5])
    if (auto E = Memory.writeInteger(Arguments[5], IRP, profile::PointerSize))
      return E;
  if (Delivery == PowerRequestDelivery::Queued || Invocation->PC) {
    const uint64_t Token = NextIRPCall++;
    IRPCalls.emplace(Token,
                     IRPCall{IRPCallKind::PowerDispatch, IRP,
                             uint32_t(requestForIRP(IRP)->StackCount - 1), true,
                             StatusPending});
    if (Delivery == PowerRequestDelivery::Queued) {
      llvm::Expected<uint64_t> ID =
          Invocation->PC
              ? Scheduler.enqueueWDMDispatch({IRP,
                                              Invocation->Argument0,
                                              profile::WorkerThreadIdentity,
                                              Invocation->PC,
                                              {Invocation->Argument0, IRP}})
              : Scheduler.enqueueWDMProviderDispatch(
                    IRP, Invocation->Argument0, profile::WorkerThreadIdentity);
      if (!ID)
        return ID.takeError();
      ScheduledModelContinuations.emplace(
          *ID, GuestCallToken{GuestCallOwner::WDM, Token});
    } else {
      PendingWdmCall = KernelGuestCall{{GuestCallOwner::WDM, Token},
                                       Invocation->PC,
                                       {Invocation->Argument0, IRP}};
    }
    if (!WaitWake)
      ++Provider->RequestedPowerIndex;
    return StatusPending;
  }
  if (!WaitWake)
    ++Provider->RequestedPowerIndex;
  auto Status = callProviderDriver(Invocation->Argument0, IRP);
  if (!Status)
    return Status.takeError();
  if (auto E = recordDispatchReturn(IRP, uint32_t(*Status)))
    return std::move(E);
  if (PendingWdmCall)
    IRPCalls.at(PendingWdmCall->Token.ID).ReturnValue = StatusPending;
  if (auto E = tryFinalizePowerRequest(IRP))
    return std::move(E);
  return StatusPending;
}

llvm::Error KernelModel::tryFinalizePowerRequest(uint64_t IRP) {
  auto *Request = requestForIRP(IRP);
  if (!Request || !Request->ChildPower || !Request->Completed ||
      !Request->DispatchReturned)
    return llvm::Error::success();
  const auto &Child = *Request->ChildPower;
  if (Child.Callback && !Child.CallbackReturned)
    return llvm::Error::success();
  if (std::any_of(IRPCalls.begin(), IRPCalls.end(),
                  [&](const auto &Entry) { return Entry.second.IRP == IRP; }))
    return llvm::Error::success();
  const uint64_t ParentIRP = Child.FrameworkParent;
  const bool PrepareSleep = Child.PrepareSystemSleep;
  const auto Status = Result.Requests[Request->ResultIndex].IOStatus;
  if (auto E = finalizeRequest(IRP))
    return E;
  if (ParentIRP) {
    auto *Parent = requestForIRP(ParentIRP);
    if (!Parent || !Parent->FrameworkPolicyIssued ||
        !Parent->FrameworkTransitionAwaiting || !Status)
      return powerError("framework power child lost its retained system IRP");
    if (PrepareSleep && !(*Status & profile::NTStatusFailureMask)) {
      Parent->FrameworkPolicyIssued = false;
      auto Continued = beginFrameworkPowerPolicy(ParentIRP);
      if (!Continued)
        return Continued.takeError();
      if (!*Continued)
        return powerError(
            "sleep preparation lost its remaining device transition");
      return llvm::Error::success();
    }
    Parent->FrameworkTransitionAwaiting = false;
    if (Parent->PowerOperation->Minor == DevicePowerRequest::Set &&
        (*Status & profile::NTStatusFailureMask)) {
      const auto &Observation = *Result.Requests[Parent->ResultIndex].Power;
      if (auto E = Framework->systemPowerPolicy(Parent->PnpDevice,
                                                Observation.SystemStateBefore !=
                                                    SystemPowerState::Working))
        return E;
    }
    if (Parent->PowerOperation->Minor == DevicePowerRequest::Query ||
        (*Status & profile::NTStatusFailureMask))
      if (auto E = Memory.writeInteger(ParentIRP + windows::IRPStatusOffset,
                                       *Status, sizeof(uint32_t)))
        return E;
    return completeRequest(ParentIRP, 0);
  }
  return llvm::Error::success();
}

llvm::Expected<std::optional<uint64_t>>
KernelModel::finishPowerCompletion(uint64_t Token) {
  auto Call = IRPCalls.find(Token);
  if (Call == IRPCalls.end() ||
      Call->second.Kind != IRPCallKind::PowerCompletion ||
      !Call->second.AwaitingCallback)
    return powerError("unknown or inactive PowerCompletion continuation");
  const uint64_t IRP = Call->second.IRP;
  auto *Request = requestForIRP(IRP);
  if (!Request || !Request->Completed || !Request->ChildPower)
    return powerError("PowerCompletion lost its completed child identity");
  auto &Child = *Request->ChildPower;
  if (!Child.CallbackStarted || Child.CallbackReturned || !Child.StatusBlock)
    return powerError("PowerCompletion lost its status snapshot");
  if (auto E = prepareReleaseRange(Child.StatusBlock, PowerStatusBlockSize))
    return E;
  FreedRanges.emplace(Child.StatusBlock, PowerStatusBlockSize);
  Child.CallbackReturned = true;
  const uint64_t ReturnValue = Call->second.ReturnValue;
  IRPCalls.erase(Call);
  if (auto E = tryFinalizePowerRequest(IRP))
    return E;
  // REQUEST_POWER_COMPLETE is void; its guest RAX is never a completion status.
  return std::optional<uint64_t>{ReturnValue};
}
} // namespace neverd::emulation
