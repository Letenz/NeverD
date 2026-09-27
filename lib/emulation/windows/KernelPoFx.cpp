//===- KernelPoFx.cpp - Component power ownership and callback protocol --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Own component conditions, callback acknowledgements and bounded PoFx
/// transitions.
///
//===----------------------------------------------------------------------===//

#include "KernelPoFx.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <limits>
#include <set>

namespace neverd::emulation {
namespace {
llvm::Error powerError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "PoFx: " + Message);
}

bool isComponentCallback(KernelPoFx::CallbackKind Kind) {
  using KindType = KernelPoFx::CallbackKind;
  return Kind == KindType::ActiveCondition || Kind == KindType::IdleCondition ||
         Kind == KindType::IdleState;
}
} // namespace

llvm::Error
KernelPoFx::canRegisterDevice(uint64_t Handle,
                              const Registration &Description) const {
  if (!Handle || !Description.PDO)
    return powerError(
        "registration requires nonzero handle and PDO identities");
  if (Devices.contains(Handle) || handleForPDO(Description.PDO))
    return powerError("device or registration handle is already registered");
  if (Devices.size() >= Bounds.MaxDevices)
    return powerError("device registration bound exhausted");
  if (Description.Version != pofx::Version1)
    return powerError("only PO_FX_DEVICE version 1 is supported");
  if (Description.Components.empty() ||
      Description.Components.size() > Bounds.MaxComponents)
    return powerError("component count is zero or exceeds the execution bound");
  if (Description.Owner != RegistrationOwner::Driver &&
      Description.Owner != RegistrationOwner::Framework)
    return powerError("registration has an invalid owner");
  const bool FrameworkOwned = Description.Owner == RegistrationOwner::Framework;
  if (FrameworkOwned && Description.Components.size() != 1)
    return powerError("framework registration requires exactly one component");
  const auto &Routines = Description.Routines;
  if (Routines.PowerControl)
    return powerError(
        "PEP power-control callbacks require an explicit provider");
  if (!FrameworkOwned && bool(Routines.DevicePowerRequired) !=
                             bool(Routines.DevicePowerNotRequired))
    return powerError("device power callbacks require a paired return path");
  if (FrameworkOwned &&
      (Routines.DevicePowerRequired || Routines.DevicePowerNotRequired))
    return powerError("framework registration owns device-power callbacks");

  std::set<std::array<uint8_t, pofx::ComponentIDSize>> IDs;
  for (const auto &C : Description.Components) {
    if (C.IdleStates.empty() || C.IdleStates.size() > Bounds.MaxIdleStates)
      return powerError(
          "idle-state count is zero or exceeds the execution bound");
    if (C.DeepestWakeableState >= C.IdleStates.size())
      return powerError("deepest wakeable state is outside the component");
    if (C.IdleStates.front().TransitionLatency ||
        C.IdleStates.front().ResidencyRequirement)
      return powerError(
          "F0 latency and residency requirement must both be zero");
    if (C.IdleStates.size() > 1 && !FrameworkOwned &&
        (!Routines.ActiveCondition || !Routines.IdleCondition ||
         !Routines.IdleState))
      return powerError("multiple Fx states require all component callbacks");
    if (llvm::any_of(C.ID, [](uint8_t Byte) { return Byte != 0; }) &&
        !IDs.insert(C.ID).second)
      return powerError("nonzero component identities must be unique");
  }
  return llvm::Error::success();
}

llvm::Error KernelPoFx::registerDevice(uint64_t Handle,
                                       Registration Description) {
  if (auto Error = canRegisterDevice(Handle, Description))
    return Error;
  Device D;
  D.Components.resize(Description.Components.size());
  D.Description = std::move(Description);
  Devices.emplace(Handle, std::move(D));
  return llvm::Error::success();
}

const KernelPoFx::Registration *
KernelPoFx::registration(uint64_t Handle) const {
  const auto Found = Devices.find(Handle);
  return Found == Devices.end() ? nullptr : &Found->second.Description;
}

std::optional<uint64_t> KernelPoFx::handleForPDO(uint64_t PDO) const {
  for (const auto &[Handle, D] : Devices)
    if (D.Description.PDO == PDO)
      return Handle;
  return std::nullopt;
}

uint64_t KernelPoFx::callbackCount() const {
  uint64_t Count = 0;
  for (const auto &[Handle, D] : Devices)
    Count += D.Callbacks.size();
  return Count;
}

llvm::Error
KernelPoFx::mutate(uint64_t Handle,
                   llvm::function_ref<llvm::Error(Device &)> Change) {
  auto Found = Devices.find(Handle);
  if (Found == Devices.end())
    return powerError("operation requires a live registration handle");
  Device Candidate = Found->second;
  uint64_t Next = NextToken;
  if (auto Error = Change(Candidate))
    return Error;
  if (auto Error = reconcile(Handle, Candidate, Next))
    return Error;
  const uint64_t OtherCallbacks =
      callbackCount() - Found->second.Callbacks.size();
  if (Candidate.Callbacks.size() > Bounds.MaxCallbacks ||
      OtherCallbacks > Bounds.MaxCallbacks - Candidate.Callbacks.size())
    return powerError("pending callback bound exhausted");
  Found->second = std::move(Candidate);
  NextToken = Next;
  return llvm::Error::success();
}

llvm::Error KernelPoFx::enqueue(uint64_t Handle, Device &D, CallbackKind Kind,
                                uint32_t Index, uint32_t State, uint64_t Thread,
                                uint64_t &Next) {
  if (Next == std::numeric_limits<uint64_t>::max())
    return powerError("callback identity bound exhausted");
  const auto &Description = D.Description;
  const auto &Routines = Description.Routines;
  Callback Call;
  Call.Token = Next++;
  Call.Handle = Handle;
  Call.PDO = Description.PDO;
  Call.Thread = Thread;
  Call.Kind = Kind;
  Call.Component = Index;
  Call.State = State;
  Call.Arguments = {Description.Context};
  switch (Kind) {
  case CallbackKind::ActiveCondition:
    Call.PC = Routines.ActiveCondition;
    break;
  case CallbackKind::IdleCondition:
    Call.PC = Routines.IdleCondition;
    break;
  case CallbackKind::IdleState:
    Call.PC = Routines.IdleState;
    break;
  case CallbackKind::DevicePowerRequired:
    Call.PC = Routines.DevicePowerRequired;
    break;
  case CallbackKind::DevicePowerNotRequired:
    Call.PC = Routines.DevicePowerNotRequired;
    break;
  }
  Call.Internal = Description.Owner == RegistrationOwner::Framework && !Call.PC;
  if (!Call.PC && !Call.Internal)
    return powerError("transition requires a registered callback");
  if (isComponentCallback(Kind)) {
    Call.Arguments.push_back(Index);
    if (Kind == CallbackKind::IdleState)
      Call.Arguments.push_back(State);
    D.Components[Index].CallbackToken = Call.Token;
    D.Components[Index].TransitionPending = true;
  } else {
    D.PowerCallback = Call.Token;
  }
  D.Callbacks.emplace(Call.Token, CallbackState{std::move(Call)});
  return llvm::Error::success();
}

bool KernelPoFx::allIdle(const Device &D) const {
  return llvm::all_of(D.Components, [](const ComponentState &C) {
    return !C.Active && !C.References && !C.CallbackToken &&
           !C.RequestedIdleState && !C.BlockingActivationThread;
  });
}

llvm::Error KernelPoFx::reconcile(uint64_t Handle, Device &D, uint64_t &Next) {
  if (!D.Started || D.Quiescing)
    return llvm::Error::success();
  const bool HasReferences =
      llvm::any_of(D.Components, [](const ComponentState &C) {
        return C.References || C.BlockingActivationThread;
      });
  if (HasReferences) {
    D.IdleSince.reset();
    D.PowerDownDeadline.reset();
    if (!D.PowerRequired && !D.PowerCallback) {
      uint64_t Thread = 0;
      for (const auto &C : D.Components)
        if (C.BlockingActivationThread || (C.References && C.CallbackThread)) {
          Thread = C.BlockingActivationThread ? C.BlockingActivationThread
                                              : C.CallbackThread;
          break;
        }
      if (auto Error = enqueue(Handle, D, CallbackKind::DevicePowerRequired, 0,
                               0, Thread, Next))
        return Error;
    }
  }
  if (D.PowerCallback || !D.PowerRequired)
    return llvm::Error::success();

  const bool FrameworkOwned =
      D.Description.Owner == RegistrationOwner::Framework;
  const auto &Routines = D.Description.Routines;
  for (uint32_t Index = 0; Index != D.Components.size(); ++Index) {
    auto &C = D.Components[Index];
    if (C.CallbackToken)
      continue;
    if (C.References || C.BlockingActivationThread) {
      C.RequestedIdleState.reset();
      const uint64_t Thread = C.BlockingActivationThread
                                  ? C.BlockingActivationThread
                                  : C.CallbackThread;
      if (C.IdleState) {
        if (auto Error = enqueue(Handle, D, CallbackKind::IdleState, Index, 0,
                                 Thread, Next))
          return Error;
      } else if (!C.Active) {
        if (Routines.ActiveCondition || FrameworkOwned) {
          if (auto Error = enqueue(Handle, D, CallbackKind::ActiveCondition,
                                   Index, 0, Thread, Next))
            return Error;
        } else {
          if (auto Error = completeCondition(C, true))
            return Error;
        }
      }
    }
    if (!C.CallbackToken && !C.References && !C.BlockingActivationThread &&
        C.Active) {
      if (Routines.IdleCondition || FrameworkOwned) {
        if (auto Error = enqueue(Handle, D, CallbackKind::IdleCondition, Index,
                                 0, C.CallbackThread, Next))
          return Error;
      } else {
        if (auto Error = completeCondition(C, false))
          return Error;
      }
    } else if (!C.CallbackToken && !C.References &&
               !C.BlockingActivationThread && C.RequestedIdleState) {
      const uint32_t State = *C.RequestedIdleState;
      if (auto Error = validateIdleState(D, Index, State))
        return Error;
      C.RequestedIdleState.reset();
      if (State != C.IdleState)
        if (auto Error = enqueue(Handle, D, CallbackKind::IdleState, Index,
                                 State, 0, Next))
          return Error;
    }
    if (!C.CallbackToken)
      C.CallbackThread = 0;
  }
  if (allIdle(D)) {
    if (!D.IdleSince)
      D.IdleSince = Now;
    if (D.PowerDownDeadline && *D.PowerDownDeadline <= Now) {
      D.PowerDownDeadline.reset();
      if (auto Error = enqueue(Handle, D, CallbackKind::DevicePowerNotRequired,
                               0, 0, 0, Next))
        return Error;
    }
  } else {
    D.IdleSince.reset();
    D.PowerDownDeadline.reset();
  }
  return llvm::Error::success();
}

llvm::Error KernelPoFx::start(uint64_t Handle) {
  return mutate(Handle, [](Device &D) -> llvm::Error {
    if (D.Quiescing)
      return powerError("framework registration is quiescing");
    if (D.Started)
      return powerError("power management is already started");
    D.Started = true;
    return llvm::Error::success();
  });
}

llvm::Error KernelPoFx::activate(uint64_t Handle, uint32_t Index,
                                 uint64_t CallbackThread) {
  return mutate(Handle, [&](Device &D) -> llvm::Error {
    if (D.Quiescing)
      return powerError("framework registration is quiescing");
    if (Index >= D.Components.size())
      return powerError("component index is outside the registration");
    auto &C = D.Components[Index];
    if (C.References == std::numeric_limits<uint64_t>::max())
      return powerError("component activation reference count overflow");
    if (CallbackThread && (C.CallbackToken || C.BlockingActivationThread))
      return powerError("blocking activation overlaps an existing transition");
    if (CallbackThread && !C.Active)
      C.BlockingActivationThread = CallbackThread;
    if (!C.References)
      C.CallbackThread = CallbackThread;
    ++C.References;
    return llvm::Error::success();
  });
}

llvm::Error KernelPoFx::idle(uint64_t Handle, uint32_t Index,
                             uint64_t CallbackThread) {
  return mutate(Handle, [&](Device &D) -> llvm::Error {
    if (D.Quiescing)
      return powerError("framework registration is quiescing");
    if (Index >= D.Components.size())
      return powerError("component index is outside the registration");
    auto &C = D.Components[Index];
    if (!C.References)
      return powerError("component idle has no matching activation reference");
    if (CallbackThread && (C.CallbackToken || C.BlockingActivationThread))
      return powerError("blocking idle overlaps an existing transition");
    --C.References;
    if (!C.References)
      C.CallbackThread = CallbackThread;
    return llvm::Error::success();
  });
}

llvm::Error KernelPoFx::completeCondition(ComponentState &C, bool Active) {
  auto &Generation = Active ? C.ActiveGeneration : C.IdleGeneration;
  if (Generation == std::numeric_limits<uint64_t>::max())
    return powerError("condition completion identity bound exhausted");
  ++Generation;
  C.Active = Active;
  if (Active)
    C.BlockingActivationThread = 0;
  return llvm::Error::success();
}

llvm::Error KernelPoFx::retireCallback(Device &D, uint64_t Token) {
  const auto Found = D.Callbacks.find(Token);
  if (Found == D.Callbacks.end())
    return powerError("callback identity is no longer live");
  const auto &State = Found->second;
  if (!State.Returned || !State.Completed)
    return llvm::Error::success();
  const auto &Call = State.Call;
  if (isComponentCallback(Call.Kind)) {
    auto &C = D.Components[Call.Component];
    if (Call.Kind == CallbackKind::IdleState)
      C.IdleState = Call.State;
    else if (auto Error = completeCondition(
                 C, Call.Kind == CallbackKind::ActiveCondition))
      return Error;
    C.CallbackToken.reset();
    C.TransitionPending = false;
  } else {
    D.PowerRequired = Call.Kind == CallbackKind::DevicePowerRequired;
    D.PowerCallback.reset();
  }
  D.Callbacks.erase(Found);
  return llvm::Error::success();
}

llvm::Error KernelPoFx::complete(uint64_t Handle, uint32_t Index,
                                 CallbackKind Kind) {
  return mutate(Handle, [&](Device &D) -> llvm::Error {
    if (Index >= D.Components.size())
      return powerError("component index is outside the registration");
    const auto Token = D.Components[Index].CallbackToken;
    if (!Token)
      return powerError("component has no pending callback to complete");
    auto &State = D.Callbacks.at(*Token);
    if (State.Call.Kind != Kind || !State.Begun || State.Completed)
      return powerError("completion requires the matching delivered callback");
    State.Completed = true;
    return retireCallback(D, *Token);
  });
}

llvm::Error KernelPoFx::completeIdleCondition(uint64_t Handle, uint32_t Index) {
  return complete(Handle, Index, CallbackKind::IdleCondition);
}

llvm::Error KernelPoFx::completeIdleState(uint64_t Handle, uint32_t Index) {
  return complete(Handle, Index, CallbackKind::IdleState);
}

llvm::Error KernelPoFx::completePower(uint64_t Handle, CallbackKind Kind) {
  return mutate(Handle, [&](Device &D) -> llvm::Error {
    if (!D.PowerCallback)
      return powerError("device has no pending power callback to complete");
    const uint64_t Token = *D.PowerCallback;
    auto &State = D.Callbacks.at(Token);
    if (State.Call.Kind != Kind || !State.Begun || State.Completed)
      return powerError("completion requires the matching delivered callback");
    State.Completed = true;
    return retireCallback(D, Token);
  });
}

llvm::Error KernelPoFx::completeDevicePowerNotRequired(uint64_t Handle) {
  return completePower(Handle, CallbackKind::DevicePowerNotRequired);
}

llvm::Error KernelPoFx::reportDevicePoweredOn(uint64_t Handle) {
  return completePower(Handle, CallbackKind::DevicePowerRequired);
}

llvm::Error KernelPoFx::setLatency(uint64_t Handle, uint32_t Index,
                                   uint64_t Latency) {
  return mutate(Handle, [&](Device &D) -> llvm::Error {
    if (Index >= D.Components.size())
      return powerError("component index is outside the registration");
    D.Components[Index].Latency = Latency;
    return llvm::Error::success();
  });
}

llvm::Error KernelPoFx::setResidency(uint64_t Handle, uint32_t Index,
                                     uint64_t Residency) {
  return mutate(Handle, [&](Device &D) -> llvm::Error {
    if (Index >= D.Components.size())
      return powerError("component index is outside the registration");
    D.Components[Index].Residency = Residency;
    return llvm::Error::success();
  });
}

llvm::Error KernelPoFx::setWake(uint64_t Handle, uint32_t Index, bool Wake) {
  return mutate(Handle, [&](Device &D) -> llvm::Error {
    if (Index >= D.Components.size())
      return powerError("component index is outside the registration");
    D.Components[Index].Wake = Wake;
    return llvm::Error::success();
  });
}

llvm::Error KernelPoFx::setDeviceIdleTimeout(uint64_t Handle,
                                             uint64_t Timeout100ns) {
  return mutate(Handle, [&](Device &D) -> llvm::Error {
    if (D.PowerDownDeadline) {
      if (Timeout100ns > std::numeric_limits<uint64_t>::max() - *D.IdleSince)
        return powerError("device idle deadline overflows virtual time");
      D.PowerDownDeadline = *D.IdleSince + Timeout100ns;
    }
    D.IdleTimeout = Timeout100ns;
    return llvm::Error::success();
  });
}

llvm::Error KernelPoFx::validateIdleState(const Device &D, uint32_t Index,
                                          uint32_t State) const {
  if (Index >= D.Components.size())
    return powerError("component index is outside the registration");
  const auto &Description = D.Description.Components[Index];
  const auto &C = D.Components[Index];
  if (State >= Description.IdleStates.size())
    return powerError("requested Fx state is outside the component");
  if (C.Wake && State > Description.DeepestWakeableState)
    return powerError("requested Fx state cannot satisfy the wake hint");
  const auto &Target = Description.IdleStates[State];
  if (State && (Target.TransitionLatency == pofx::UnknownTime ||
                Target.ResidencyRequirement == pofx::UnknownTime))
    return powerError("requested Fx state has unknown timing requirements");
  if (Target.TransitionLatency > C.Latency ||
      Target.ResidencyRequirement > C.Residency)
    return powerError("requested Fx state exceeds a component timing hint");
  return llvm::Error::success();
}

llvm::Error KernelPoFx::requestIdleState(uint64_t Handle, uint32_t Index,
                                         uint32_t State) {
  return mutate(Handle, [&](Device &D) -> llvm::Error {
    if (D.Quiescing)
      return powerError("framework registration is quiescing");
    if (auto Error = validateIdleState(D, Index, State))
      return Error;
    auto &C = D.Components[Index];
    if (!D.Started || C.Active || C.References || C.CallbackToken ||
        C.BlockingActivationThread || D.PowerCallback || !D.PowerRequired)
      return powerError("Fx decision requires an idle component in powered D0");
    C.RequestedIdleState = State;
    return llvm::Error::success();
  });
}

llvm::Error KernelPoFx::requestDevicePowerNotRequired(uint64_t Handle) {
  return mutate(Handle, [&](Device &D) -> llvm::Error {
    if (D.Quiescing)
      return powerError("framework registration is quiescing");
    if (!D.Started || !allIdle(D) || !D.PowerRequired || D.PowerCallback ||
        D.PowerDownDeadline)
      return powerError("power-down decision requires a quiescent idle device");
    if (D.Description.Owner == RegistrationOwner::Driver &&
        !D.Description.Routines.DevicePowerNotRequired)
      return powerError("device has no power-not-required callback");
    if (!D.IdleSince ||
        D.IdleTimeout > std::numeric_limits<uint64_t>::max() - *D.IdleSince)
      return powerError("device idle deadline overflows virtual time");
    D.PowerDownDeadline = *D.IdleSince + D.IdleTimeout;
    return llvm::Error::success();
  });
}

llvm::Error KernelPoFx::quiesceFrameworkRegistration(uint64_t Handle) {
  return mutate(Handle, [](Device &D) -> llvm::Error {
    if (D.Description.Owner != RegistrationOwner::Framework)
      return powerError("quiescence requires a framework-owned registration");
    if (D.PowerCallback)
      return powerError("framework teardown requires the device-power callback "
                        "to complete first");
    D.Quiescing = true;
    D.IdleSince.reset();
    D.PowerDownDeadline.reset();
    for (auto &C : D.Components)
      C.RequestedIdleState.reset();
    return llvm::Error::success();
  });
}

llvm::Error KernelPoFx::process(uint64_t Now100ns) {
  if (Now100ns < Now)
    return powerError("virtual time cannot move backwards");
  // A deadline batch is committed as a unit, including callback identities.
  // Capacity failures cannot partially advance different device registrations.
  KernelPoFx Candidate = *this;
  Candidate.Now = Now100ns;
  for (const auto &[Handle, D] : Devices)
    if (auto Error = Candidate.mutate(
            Handle, [](Device &) { return llvm::Error::success(); }))
      return Error;
  *this = std::move(Candidate);
  return llvm::Error::success();
}

std::optional<uint64_t> KernelPoFx::nextDeadline() const {
  std::optional<uint64_t> Result;
  for (const auto &[Handle, D] : Devices)
    if (D.PowerDownDeadline && (!Result || *D.PowerDownDeadline < *Result))
      Result = D.PowerDownDeadline;
  return Result;
}

std::optional<KernelPoFx::Callback> KernelPoFx::nextCallback() const {
  const Callback *Result = nullptr;
  for (const auto &[Handle, D] : Devices)
    for (const auto &[Token, State] : D.Callbacks)
      if (!State.Submitted && (!Result || Token < Result->Token))
        Result = &State.Call;
  return Result ? std::optional<Callback>(*Result) : std::nullopt;
}

const KernelPoFx::Callback *KernelPoFx::callback(uint64_t Token) const {
  for (const auto &[Handle, D] : Devices) {
    const auto Found = D.Callbacks.find(Token);
    if (Found != D.Callbacks.end())
      return &Found->second.Call;
  }
  return nullptr;
}

llvm::Expected<uint64_t> KernelPoFx::callbackOwner(uint64_t Token) const {
  const auto *Call = callback(Token);
  if (!Call)
    return powerError("operation requires a live callback identity");
  return Call->Handle;
}

llvm::Error KernelPoFx::submitCallback(uint64_t Token) {
  auto Handle = callbackOwner(Token);
  if (!Handle)
    return Handle.takeError();
  return mutate(*Handle, [&](Device &D) -> llvm::Error {
    auto &State = D.Callbacks.at(Token);
    if (State.Submitted)
      return powerError("callback was already submitted");
    State.Submitted = true;
    return llvm::Error::success();
  });
}

llvm::Error KernelPoFx::beginCallback(uint64_t Token) {
  auto Handle = callbackOwner(Token);
  if (!Handle)
    return Handle.takeError();
  return mutate(*Handle, [&](Device &D) -> llvm::Error {
    auto &State = D.Callbacks.at(Token);
    if (!State.Submitted || State.Begun)
      return powerError("callback entry requires one submitted invocation");
    State.Begun = true;
    // The active notification reports an already completed hardware change.
    if (State.Call.Kind == CallbackKind::ActiveCondition)
      D.Components[State.Call.Component].Active = true;
    return llvm::Error::success();
  });
}

llvm::Error KernelPoFx::finishCallback(uint64_t Token) {
  auto Handle = callbackOwner(Token);
  if (!Handle)
    return Handle.takeError();
  return mutate(*Handle, [&](Device &D) -> llvm::Error {
    auto &State = D.Callbacks.at(Token);
    if (!State.Begun || State.Returned)
      return powerError("callback return requires one active invocation");
    State.Returned = true;
    if (State.Call.Kind == CallbackKind::ActiveCondition)
      State.Completed = true;
    return retireCallback(D, Token);
  });
}

bool KernelPoFx::hasPendingCallbacks() const { return callbackCount() != 0; }

llvm::Expected<KernelPoFx::ComponentSnapshot>
KernelPoFx::component(uint64_t Handle, uint32_t Index) const {
  const auto Found = Devices.find(Handle);
  if (Found == Devices.end())
    return powerError("operation requires a live registration handle");
  if (Index >= Found->second.Components.size())
    return powerError("component index is outside the registration");
  return Found->second.Components[Index];
}

llvm::Expected<bool> KernelPoFx::conditionReached(uint64_t Handle,
                                                  uint32_t Index,
                                                  bool Active) const {
  auto C = component(Handle, Index);
  if (!C)
    return C.takeError();
  return C->Active == Active && !C->TransitionPending &&
         (!Active || C->IdleState == 0);
}

llvm::Expected<bool> KernelPoFx::callbacksDrained(uint64_t Handle) const {
  const auto Found = Devices.find(Handle);
  if (Found == Devices.end())
    return powerError("operation requires a live registration handle");
  return Found->second.Callbacks.empty();
}

llvm::Error KernelPoFx::canUnregisterDevice(uint64_t Handle) const {
  auto Drained = callbacksDrained(Handle);
  if (!Drained)
    return Drained.takeError();
  if (!*Drained)
    return powerError("registration is retained by pending callbacks");
  return llvm::Error::success();
}

llvm::Error KernelPoFx::unregisterDevice(uint64_t Handle) {
  if (auto Error = canUnregisterDevice(Handle))
    return Error;
  Devices.erase(Handle);
  return llvm::Error::success();
}

llvm::Error KernelPoFx::canReleasePDO(uint64_t PDO) const {
  if (handleForPDO(PDO))
    return powerError("PDO is retained by its live power registration");
  return llvm::Error::success();
}
} // namespace neverd::emulation
