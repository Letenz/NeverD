//===- KernelModelPoFx.cpp - Guest PoFx ABI and callback execution -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Decode native component-power contracts and retain guest callback ownership.
///
//===----------------------------------------------------------------------===//
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "llvm/ADT/StringSwitch.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
llvm::Error poFxError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "PoFx: " + Message);
}
enum class API {
#define NEVERD_KERNEL_POFX_API(Name, Arity, IRQL) Name,
#include "KernelPoFxAPIs.def"
#undef NEVERD_KERNEL_POFX_API
  Unknown
};
} // namespace

llvm::Expected<KernelPoFx::Component>
KernelModel::readPoFxComponent(uint64_t Address) {
  if (auto E = validateGuestAccess(Address, pofx::ComponentSize, false))
    return E;
  auto StateCount =
      Memory.readInteger(Address + pofx::ComponentIdleStateCount, 4);
  if (!StateCount)
    return StateCount.takeError();
  auto Deepest =
      Memory.readInteger(Address + pofx::ComponentDeepestWakeableState, 4);
  if (!Deepest)
    return Deepest.takeError();
  auto States = Memory.readInteger(Address + pofx::ComponentIdleStates,
                                   profile::PointerSize);
  if (!States)
    return States.takeError();
  if (!*StateCount || *StateCount > pofx::DefaultMaxIdleStates)
    return poFxError("component has an invalid idle-state count");
  if (auto E = validateGuestAccess(*States, *StateCount * pofx::IdleStateSize,
                                   false))
    return E;
  KernelPoFx::Component Component;
  if (auto E = Memory.read(Address + pofx::ComponentID, Component.ID))
    return E;
  Component.DeepestWakeableState = *Deepest;
  for (uint64_t State = 0; State < *StateCount; ++State) {
    const uint64_t StateBase = *States + State * pofx::IdleStateSize;
    auto Latency =
        Memory.readInteger(StateBase + pofx::IdleStateTransitionLatency, 8);
    if (!Latency)
      return Latency.takeError();
    auto Residency =
        Memory.readInteger(StateBase + pofx::IdleStateResidencyRequirement, 8);
    if (!Residency)
      return Residency.takeError();
    auto Power = Memory.readInteger(StateBase + pofx::IdleStateNominalPower, 4);
    if (!Power)
      return Power.takeError();
    Component.IdleStates.push_back({*Latency, *Residency, uint32_t(*Power)});
  }
  return Component;
}

llvm::Expected<KernelPoFx::Registration>
KernelModel::readPoFxRegistration(uint64_t PDO, uint64_t Address) {
  if (auto E = validateGuestAccess(Address, pofx::DeviceComponents, false))
    return E;
  auto Version = Memory.readInteger(Address + pofx::DeviceVersion, 4);
  if (!Version)
    return Version.takeError();
  auto Count = Memory.readInteger(Address + pofx::DeviceComponentCount, 4);
  if (!Count)
    return Count.takeError();
  if (*Version != pofx::Version1 || !*Count ||
      *Count > pofx::DefaultMaxComponents)
    return poFxError("unsupported registration version or component count");
  const uint64_t Bytes = pofx::DeviceComponents + *Count * pofx::ComponentSize;
  if (auto E = validateGuestAccess(Address, Bytes, false))
    return E;
  KernelPoFx::Registration Registration;
  Registration.PDO = PDO;
  Registration.Version = *Version;
  struct Field {
    uint64_t Offset;
    uint64_t *Value;
  };
  Field Fields[] = {
      {pofx::DeviceActiveCondition, &Registration.Routines.ActiveCondition},
      {pofx::DeviceIdleCondition, &Registration.Routines.IdleCondition},
      {pofx::DeviceIdleState, &Registration.Routines.IdleState},
      {pofx::DevicePowerRequired, &Registration.Routines.DevicePowerRequired},
      {pofx::DevicePowerNotRequired,
       &Registration.Routines.DevicePowerNotRequired},
      {pofx::DevicePowerControl, &Registration.Routines.PowerControl},
      {pofx::DeviceContext, &Registration.Context}};
  for (auto &Field : Fields) {
    auto Value =
        Memory.readInteger(Address + Field.Offset, profile::PointerSize);
    if (!Value)
      return Value.takeError();
    *Field.Value = *Value;
  }
  for (uint64_t Index = 0; Index < *Count; ++Index) {
    auto Component = readPoFxComponent(Address + pofx::DeviceComponents +
                                       Index * pofx::ComponentSize);
    if (!Component)
      return Component.takeError();
    Registration.Components.push_back(std::move(*Component));
  }
  return Registration;
}

llvm::Error KernelModel::validatePoFxRegistrationDevice(uint64_t PDO) const {
  auto State = Lifecycle.snapshot(PDO);
  if (!State)
    return State.takeError();
  bool Running = State->Pnp == DevicePnpState::Started && !State->PnpOperation;
  DevicePowerState PhysicalPower = State->DevicePower;
  for (const auto &[IRP, Request] : Requests) {
    if (Request.PnpDevice != PDO)
      continue;
    const auto &Observation = Result.Requests[Request.ResultIndex];
    // A driver normally registers after lower START succeeds but before its
    // own completion releases the retained upper START transaction.
    if (State->PnpOperation && Request.PnpTicket == State->PnpOperation &&
        Request.PnpOperation &&
        Request.PnpOperation->Minor == DevicePnpRequest::Start &&
        Observation.Pnp && Observation.Pnp->BusCompletedAt100ns &&
        Observation.Pnp->BusStatus &&
        !(*Observation.Pnp->BusStatus & profile::NTStatusFailureMask))
      Running = true;
    // Resource-free devices still publish lower power completion before the
    // upper lifecycle transaction finishes. PoSetPowerState is independent.
    if (State->DevicePowerOperation &&
        Request.PowerTicket == State->DevicePowerOperation &&
        Request.PowerOperation &&
        Request.PowerOperation->Type == DriverPowerType::Device &&
        Request.PowerOperation->Minor == DevicePowerRequest::Set &&
        Observation.Power && Observation.Power->BusCompletedAt100ns &&
        Observation.Power->BusStatus &&
        !(*Observation.Power->BusStatus & profile::NTStatusFailureMask))
      PhysicalPower =
          static_cast<DevicePowerState>(Request.PowerOperation->State);
  }
  if (const auto *Resource = Resources.find(PDO)) {
    Running &= Resource->Present && Resource->Assigned && !Resource->Cold;
    PhysicalPower = Resource->Power;
  }
  if (!Running || PhysicalPower != DevicePowerState::D0)
    return poFxError(
        "registration requires a running D0 device after successful START");
  return llvm::Error::success();
}

llvm::Expected<uint64_t> KernelModel::callPoFxAPI(llvm::StringRef Name,
                                                  llvm::ArrayRef<uint64_t> A) {
  const API Kind = llvm::StringSwitch<API>(Name)
#define NEVERD_KERNEL_POFX_API(Symbol, Arity, IRQL) .Case(#Symbol, API::Symbol)
#include "KernelPoFxAPIs.def"
#undef NEVERD_KERNEL_POFX_API
                       .Default(API::Unknown);
  if (Kind == API::Unknown)
    return poFxError("unknown component power operation");
  if (auto E = PoFx.process(Scheduler.now100ns()))
    return E;
  if (Kind == API::PoFxRegisterDevice) {
    const auto Device = Devices.find(A[0]);
    if (Device == Devices.end() || Device->second.DeletePending)
      return poFxError("registration requires a live device object");
    auto PDO = pnpDeviceForRoute(A[0]);
    if (!PDO)
      return PDO.takeError();
    if (!*PDO)
      return poFxError("registration requires a configured PnP provider");
    if (*PDO != A[0])
      return poFxError("registration requires the exact configured PDO");
    if (auto E = validatePoFxRegistrationDevice(*PDO))
      return E;
    if (auto E = validateGuestAccess(A[2], profile::PointerSize, true))
      return E;
    auto Writable =
        Memory.canAccess(A[2], profile::PointerSize, GuestPermission::Write);
    if (!Writable)
      return Writable.takeError();
    if (!*Writable)
      return poFxError("registration output is not writable");
    auto Description = readPoFxRegistration(*PDO, A[1]);
    if (!Description)
      return Description.takeError();
    const uint64_t Candidate = (NextAllocation + profile::PointerSize - 1) &
                               ~(profile::PointerSize - 1);
    if (auto E = PoFx.canRegisterDevice(Candidate, *Description))
      return E;
    auto Handle = allocate(profile::PointerSize, profile::PointerSize);
    if (!Handle)
      return Handle.takeError();
    if (auto E = PoFx.registerDevice(*Handle, std::move(*Description)))
      return E;
    PoFxDeviceObjects.emplace(*Handle, A[0]);
    if (auto E = Memory.writeInteger(A[2], *Handle, profile::PointerSize))
      return E;
    return windows::StatusSuccess;
  }
  const auto *Registration = PoFx.registration(A[0]);
  if (!Registration)
    return poFxError("operation requires a live PoFx handle");
  if (Registration->Owner == KernelPoFx::RegistrationOwner::Framework) {
    switch (Kind) {
    case API::PoFxCompleteIdleCondition:
    case API::PoFxCompleteIdleState:
    case API::PoFxSetComponentLatency:
    case API::PoFxSetComponentResidency:
    case API::PoFxSetComponentWake:
      break;
    default:
      return poFxError(
          "operation is owned by the framework registration in this profile");
    }
  }
  llvm::Error E = llvm::Error::success();
  switch (Kind) {
  case API::PoFxRegisterDevice:
  case API::Unknown:
    llvm_unreachable("registration and lookup handled before dispatch");
  case API::PoFxUnregisterDevice:
    if (std::any_of(
            BlockingPoFx.begin(), BlockingPoFx.end(),
            [&](const auto &Entry) { return Entry.second.Handle == A[0]; }))
      return poFxError("registration is retained by a blocking caller");
    E = PoFx.unregisterDevice(A[0]);
    if (!E) {
      PoFxDeviceObjects.erase(A[0]);
      FreedRanges.emplace(A[0], profile::PointerSize);
    }
    break;
  case API::PoFxStartDevicePowerManagement:
    E = PoFx.start(A[0]);
    break;
  case API::PoFxActivateComponent:
  case API::PoFxIdleComponent: {
    const uint32_t Flags = A[2];
    if (Flags & ~(pofx::FlagBlocking | pofx::FlagAsyncOnly) ||
        Flags == (pofx::FlagBlocking | pofx::FlagAsyncOnly))
      return poFxError("component operation has invalid flags");
    const bool Blocking = Flags & pofx::FlagBlocking;
    if (Blocking &&
        (CurrentIRQL >= scheduler::DispatchLevel || !CurrentThreadKey ||
         BlockingPoFx.contains(CurrentThreadKey)))
      return poFxError("blocking component operation requires an available "
                       "caller thread below DISPATCH_LEVEL");
    const bool Active = Kind == API::PoFxActivateComponent;
    E = Active
            ? PoFx.activate(A[0], uint32_t(A[1]),
                            Blocking ? CurrentThreadKey : 0)
            : PoFx.idle(A[0], uint32_t(A[1]), Blocking ? CurrentThreadKey : 0);
    if (!E && Blocking) {
      auto Component = PoFx.component(A[0], uint32_t(A[1]));
      if (!Component)
        return Component.takeError();
      BlockingPoFxOperation Operation{
          A[0], CurrentThreadKey, uint32_t(A[1]), Active, {}};
      // Releasing a nested activation reference does not request a condition
      // change and must not wait for other users to release their references.
      Operation.Completed = !Active && Component->References != 0;
      Operation.CompletionGeneration =
          Active ? Component->ActiveGeneration : Component->IdleGeneration;
      BlockingPoFx.emplace(CurrentThreadKey, std::move(Operation));
    }
    break;
  }
  case API::PoFxCompleteIdleCondition:
    E = PoFx.completeIdleCondition(A[0], uint32_t(A[1]));
    break;
  case API::PoFxCompleteIdleState:
    E = PoFx.completeIdleState(A[0], uint32_t(A[1]));
    break;
  case API::PoFxCompleteDevicePowerNotRequired:
    E = PoFx.completeDevicePowerNotRequired(A[0]);
    break;
  case API::PoFxReportDevicePoweredOn: {
    E = PoFx.reportDevicePoweredOn(A[0]);
    break;
  }
  case API::PoFxSetComponentLatency:
    E = PoFx.setLatency(A[0], uint32_t(A[1]), A[2]);
    break;
  case API::PoFxSetComponentResidency:
    E = PoFx.setResidency(A[0], uint32_t(A[1]), A[2]);
    break;
  case API::PoFxSetComponentWake:
    E = PoFx.setWake(A[0], uint32_t(A[1]), uint8_t(A[2]) != 0);
    break;
  case API::PoFxSetDeviceIdleTimeout:
    E = PoFx.setDeviceIdleTimeout(A[0], A[1]);
    break;
  }
  if (E)
    return std::move(E);
  if (auto Error = queuePoFxCallbacks())
    return Error;
  const bool OriginalBlockingOperation =
      (Kind == API::PoFxActivateComponent || Kind == API::PoFxIdleComponent) &&
      (A[2] & pofx::FlagBlocking);
  if (OriginalBlockingOperation)
    if (auto E = waitForPoFxOperation(CurrentThreadKey))
      return E;
  return 0;
}

llvm::Error KernelModel::queuePoFxCallbacks() {
  while (auto Call = PoFx.nextCallback()) {
    if (Call->Internal) {
      if (auto E = PoFx.submitCallback(Call->Token))
        return E;
      if (auto E = PoFx.beginCallback(Call->Token))
        return E;
      if (auto E = processInternalPoFxCallback(*Call))
        return E;
      if (auto E = PoFx.finishCallback(Call->Token))
        return E;
      continue;
    }
    if (Call->Thread) {
      auto Operation = BlockingPoFx.find(Call->Thread);
      if (Operation == BlockingPoFx.end())
        return poFxError("same-thread callback lost its blocking operation");
      if (auto E = PoFx.submitCallback(Call->Token))
        return E;
      const auto TerminalKind = Operation->second.Active
                                    ? KernelPoFx::CallbackKind::ActiveCondition
                                    : KernelPoFx::CallbackKind::IdleCondition;
      if (Call->Kind == TerminalKind)
        Operation->second.TerminalCallback = Call->Token;
      Operation->second.Calls.push_back(
          {{GuestCallOwner::PoFx, Call->Token}, Call->PC, Call->Arguments});
      continue;
    }
    const auto Owner = PoFxDeviceObjects.find(Call->Handle);
    if (Owner == PoFxDeviceObjects.end())
      return poFxError("callback lost its device-object reference");
    KernelScheduler::Callback Invocation;
    Invocation.Object = Call->Token;
    Invocation.Owner = Owner->second;
    Invocation.Thread = profile::WorkerThreadIdentity;
    Invocation.PC = Call->PC;
    Invocation.Arguments = Call->Arguments;
    auto ID = Scheduler.enqueuePoFx(std::move(Invocation));
    if (!ID)
      return ID.takeError();
    if (auto E = PoFx.submitCallback(Call->Token))
      return E;
    ScheduledModelContinuations.emplace(
        *ID, GuestCallToken{GuestCallOwner::PoFx, Call->Token});
  }
  for (auto &[Thread, Operation] : BlockingPoFx) {
    auto Component = PoFx.component(Operation.Handle, Operation.Component);
    if (!Component)
      return Component.takeError();
    const uint64_t Generation = Operation.Active ? Component->ActiveGeneration
                                                 : Component->IdleGeneration;
    if (Generation != Operation.CompletionGeneration ||
        (Operation.TerminalCallback &&
         !PoFx.callback(*Operation.TerminalCallback)))
      Operation.Completed = true;
  }
  return llvm::Error::success();
}

std::optional<KernelGuestCall>
KernelModel::takePoFxThreadCall(uint64_t Thread) {
  auto Operation = BlockingPoFx.find(Thread);
  if (Operation == BlockingPoFx.end() || Operation->second.Calls.empty())
    return std::nullopt;
  auto Call = std::move(Operation->second.Calls.front());
  Operation->second.Calls.pop_front();
  return Call;
}

llvm::Error KernelModel::waitForPoFxOperation(uint64_t Thread) {
  auto It = BlockingPoFx.find(Thread);
  if (It == BlockingPoFx.end())
    return poFxError("blocking continuation lost its caller");
  const auto &Operation = It->second;
  if (!Operation.Calls.empty())
    return llvm::Error::success();
  auto Ready = PoFx.conditionReached(Operation.Handle, Operation.Component,
                                     Operation.Active);
  if (!Ready)
    return Ready.takeError();
  if (Operation.Completed || *Ready) {
    BlockingPoFx.erase(It);
    return llvm::Error::success();
  }
  if (PendingWait)
    return poFxError("blocking continuation cannot replace a wait");
  PendingWait =
      Wait{Operation.Active ? Wait::Kind::PoFxActive : Wait::Kind::PoFxIdle,
           Operation.Handle, CurrentExecution, Thread, CurrentIRQL};
  return llvm::Error::success();
}

llvm::Expected<std::optional<uint64_t>>
KernelModel::finishPoFxCall(uint64_t Token) {
  const auto *Call = PoFx.callback(Token);
  if (!Call)
    return poFxError("callback return requires a live invocation");
  const uint64_t Thread = Call->Thread;
  if (auto E = PoFx.finishCallback(Token))
    return E;
  if (auto E = queuePoFxCallbacks())
    return E;
  if (Framework)
    if (auto E = Framework->resumePoFxTransitions())
      return E;
  if (Thread) {
    if (auto E = waitForPoFxOperation(Thread))
      return E;
    auto Operation = BlockingPoFx.find(Thread);
    if (Operation != BlockingPoFx.end() && !Operation->second.Calls.empty())
      return std::optional<uint64_t>{};
  }
  return std::optional<uint64_t>{0};
}
} // namespace neverd::emulation
