//===- KernelFrameworkPowerPolicy.cpp - Explicit idle and wake policy ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "KernelFramework.h"
#include "KernelScheduler.h"
#include "WindowsKernelLayout.h"

#include "neverd/emulation/DriverProfile.h"

#include <algorithm>
#include <array>

namespace neverd::emulation {
namespace {
llvm::Error policyError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "KMDF power policy: " + Text);
}
} // namespace

bool KernelFramework::powerPolicyBusy(uint64_t Device) const {
  for (const auto &[Handle, Request] : Requests) {
    auto Queue = Queues.find(Request.Queue);
    if (!Request.Completed && Request.Device == Device &&
        Queue != Queues.end() && Queue->second.PowerManaged)
      return true;
  }
  return false;
}

llvm::Error KernelFramework::restartIdleTimer(uint64_t Device) {
  auto &D = Devices.at(Device);
  auto &P = D.Policy;
  if (!P.Idle) {
    P.Deadline.reset();
    return llvm::Error::success();
  }
  const bool Active = !P.IdleSince || !P.Idle->Enabled || P.References ||
                      powerPolicyBusy(Device);
  const bool CanIdle = !P.SystemSleeping && D.InD0 && !D.PowerQueuesHeld;
  if (P.Idle->systemManaged()) {
    // A device that has already entered Dx remains idle until there is actual
    // activity. Its inability to start another timer is not a PoFx activation.
    if (P.Started && (Active || CanIdle)) {
      if (!PowerHost.ManagedIdle)
        return policyError("system-managed idle requires the PoFx authority");
      if (auto E = PowerHost.ManagedIdle(D.Wdm, !Active, P.Idle->Timeout100ns))
        return E;
      if (auto E = holdForPoFxComponent(Device))
        return E;
    }
    if (Active)
      P.ManagedPowerNotRequired = false;
    P.Deadline.reset();
    return llvm::Error::success();
  }
  if (Active || !CanIdle) {
    P.Deadline.reset();
    return llvm::Error::success();
  }
  if (!PowerHost.Now)
    return policyError("idle timing requires the scheduler clock");
  const uint64_t Now = PowerHost.Now();
  if (P.Idle->Timeout100ns > uint64_t(INT64_MAX) - Now)
    return policyError("idle timeout exceeds the virtual clock range");
  P.IdleSince = Now;
  P.Deadline = Now + P.Idle->Timeout100ns;
  return llvm::Error::success();
}

llvm::Expected<std::optional<uint64_t>>
KernelFramework::callPowerPolicy(llvm::StringRef Name, Binding &B,
                                 llvm::ArrayRef<uint64_t> A, uint8_t IRQL) {
  using namespace framework;
  namespace policy = power_policy;
  using Result = std::optional<uint64_t>;
  const bool Register = Name == api::WdfDeviceInitSetPowerPolicyEventCallbacks;
  const bool Idle = Name == api::WdfDeviceAssignS0IdleSettings;
  const bool Wake = Name == api::WdfDeviceAssignSxWakeSettings;
  const bool Stop = Name == api::WdfDeviceStopIdleNoTrack ||
                    Name == api::WdfDeviceStopIdleActual;
  const bool Resume = Name == api::WdfDeviceResumeIdleNoTrack ||
                      Name == api::WdfDeviceResumeIdleActual;
  if (!Register && !Idle && !Wake && !Stop && !Resume)
    return Result{};
  if (IRQL > scheduler::DispatchLevel ||
      ((Register || (Stop && uint8_t(A[2]))) && IRQL))
    return policyError(
        "operation requires its documented passive/dispatch IRQL");
  if (Register) {
    auto Init = DeviceInits.find(A[1]);
    if (Init == DeviceInits.end() || Init->second.Binding != B.Globals ||
        Init->second.Kind != DeviceInitKind::Pnp)
      return policyError(
          "callbacks require the binding's live PnP initializer");
    if (auto E = ValidateAccess(A[2], policy::CallbacksSize, false))
      return E;
    auto Size = read(A[2], 4);
    if (!Size)
      return Size.takeError();
    if (*Size != policy::CallbacksSize)
      return policyError("unsupported power-policy callback structure size");
    KernelPowerPolicy::Callbacks Callbacks;
#define NEVERD_POWER_POLICY_CALLBACK(Field, Index, ResultKind)                 \
  {                                                                            \
    auto Value =                                                               \
        read(A[2] + policy::CallbacksFirst + Index * sizeof(uint64_t));        \
    if (!Value)                                                                \
      return Value.takeError();                                                \
    Callbacks.Field = *Value;                                                  \
  }
#include "KernelPowerPolicyCallbacks.def"
#undef NEVERD_POWER_POLICY_CALLBACK
    if (Callbacks.ArmWakeFromSx && Callbacks.ArmWakeFromSxWithReason)
      return policyError("Sx arm callbacks are mutually exclusive");
    Init->second.PowerCallbacks = Callbacks;
    return Result{0};
  }
  auto D = Devices.find(A[1]);
  auto O = Objects.find(A[1]);
  if (D == Devices.end() || O == Objects.end() || O->second.Deleting ||
      O->second.Binding != B.Globals || !D->second.PDO)
    return policyError("operation requires the binding's live PnP device");
  if (!D->second.PowerPolicyOwner)
    return Resume
               ? llvm::Expected<Result>(
                     policyError("ResumeIdle requires power-policy ownership"))
               : llvm::Expected<Result>(
                     Result{Idle || Wake ? ControlInvalidDeviceRequest
                                         : policy::StatusInvalidDeviceState});
  auto &P = D->second.Policy;
  if (Idle || Wake) {
    const uint64_t Size = Idle ? policy::IdleSize : policy::WakeSize;
    if (auto E = ValidateAccess(A[2], Size, false))
      return E;
    std::array<uint32_t, policy::IdleSize / sizeof(uint32_t)> Fields{};
    for (size_t I = 0; I < Size / sizeof(uint32_t); ++I) {
      auto Value = read(A[2] + I * sizeof(uint32_t), sizeof(uint32_t));
      if (!Value)
        return Value.takeError();
      Fields[I] = *Value;
    }
    if (Fields[0] != Size)
      return Result{InfoLengthMismatch};
    if (P.Armed != KernelPowerPolicy::WakeSource::None ||
        std::any_of(PnpTransitions.begin(), PnpTransitions.end(),
                    [&](const auto &Transition) {
                      return Transition.second.Device == A[1] &&
                             !Transition.second.Entering;
                    }))
      return policyError(
          "settings cannot replace an armed or transitioning policy");
    const auto Field = [&](uint64_t Offset) {
      return Fields[Offset / sizeof(uint32_t)];
    };
    if (Idle) {
      if (Field(policy::IdleCapabilities) != policy::CannotWake &&
          Field(policy::IdleCapabilities) != policy::CanWake)
        return policyError("USB and unknown idle capabilities are unsupported");
      if (Field(policy::IdleDxState) != uint32_t(DevicePowerState::D3) ||
          (Field(policy::IdleTimeoutType) == policy::DriverManagedTimeout &&
           !Field(policy::IdleTimeout)) ||
          Field(policy::IdleTimeoutType) >
              policy::SystemManagedTimeoutWithHint ||
          Field(policy::IdleUserControl) != policy::NoUserControl ||
          Field(policy::IdleExcludeD3Cold) > policy::UseDefault)
        return policyError("idle requires explicit D3, a valid timeout policy "
                           "and no user override");
      const bool Managed =
          Field(policy::IdleTimeoutType) != policy::DriverManagedTimeout;
      if (Managed && !PowerHost.ManagedIdle)
        return policyError("system-managed idle requires the PoFx authority");
      if (P.Idle && P.Idle->TimeoutType != Field(policy::IdleTimeoutType))
        return policyError(
            "idle timeout type cannot change after initial assignment");
      if (Managed && !P.Idle && (P.Started || D->second.InD0))
        return policyError(
            "system-managed policy must precede first D0 entry completion");
      if (Field(policy::IdleEnabled) > policy::UseDefault ||
          Field(policy::IdlePowerUpOnSystemWake) > policy::UseDefault)
        return Result{windows::StatusInvalidParameter};
      if (Field(policy::IdleCapabilities) == policy::CanWake) {
        if (!PowerHost.CanWake)
          return policyError(
              "idle wake settings require provider capabilities");
        auto Capable = PowerHost.CanWake(D->second.Wdm, false);
        if (!Capable)
          return Capable.takeError();
        if (!*Capable)
          return Result{policy::StatusPowerStateInvalid};
      }
      const auto Previous = P.Idle;
      P.Idle = KernelPowerPolicy::IdleSettings{
          Field(policy::IdleEnabled) != 0,
          Field(policy::IdleCapabilities) == policy::CanWake,
          Previous ? Previous->PowerUpOnSystemWake
                   : Field(policy::IdlePowerUpOnSystemWake) == policy::True,
          Field(policy::IdleTimeoutType) == policy::SystemManagedTimeout
              ? 0
              : uint64_t(Field(policy::IdleTimeout)) *
                    policy::TicksPerMillisecond,
          uint32_t(Field(policy::IdleTimeoutType)),
          Previous ? Previous->ExcludeD3Cold
                   : uint32_t(Field(policy::IdleExcludeD3Cold))};
      if (auto E = restartIdleTimer(A[1])) {
        P.Idle = Previous;
        return E;
      }
      if (!P.Idle->Enabled && P.Started && !D->second.InD0)
        P.PowerUpRequested = true;
    } else {
      if (Field(policy::WakeDxState) != uint32_t(DevicePowerState::D3) ||
          Field(policy::WakeUserControl) != policy::NoUserControl ||
          uint16_t(Fields[policy::WakeChildren / sizeof(uint32_t)]))
        return policyError(
            "wake requires explicit D3 and no user or child-device policy");
      if (Field(policy::WakeEnabled) > policy::UseDefault)
        return Result{windows::StatusInvalidParameter};
      if (Field(policy::WakeEnabled)) {
        if (!PowerHost.CanWake)
          return policyError(
              "system wake settings require provider capabilities");
        auto Capable = PowerHost.CanWake(D->second.Wdm, true);
        if (!Capable)
          return Capable.takeError();
        if (!*Capable)
          return Result{policy::StatusPowerStateInvalid};
      }
      P.Wake = KernelPowerPolicy::WakeSettings{Field(policy::WakeEnabled) != 0};
    }
    return Result{0};
  }
  if (Stop) {
    if (P.Failure)
      return Result{policy::StatusPowerStateInvalid};
    if (!P.Started && !D->second.InD0)
      return policyError("StopIdle precedes the first successful D0 entry");
    if (P.References == UINT64_MAX)
      return policyError("idle reference count overflow");
    const bool Wait = uint8_t(A[2]) != 0;
    for (const auto &[Token, Transition] : PnpTransitions)
      if (Wait && Transition.Device == A[1] && !Transition.Entering)
        return policyError("StopIdle(TRUE) during power down would deadlock");
    if (P.Idle && P.Idle->systemManaged())
      if (auto E =
              PowerHost.ManagedIdle(D->second.Wdm, false, P.Idle->Timeout100ns))
        return E;
    if (auto E = holdForPoFxComponent(A[1]))
      return E;
    ++P.References;
    P.Deadline.reset();
    P.ManagedPowerNotRequired = false;
    if (D->second.InD0 && !D->second.PowerQueuesHeld)
      return Result{windows::StatusSuccess};
    P.PowerUpRequested = true;
    return Result{windows::StatusPending};
  }
  if (!P.References)
    return policyError("ResumeIdle has no matching successful StopIdle");
  --P.References;
  if (auto E = restartIdleTimer(A[1])) {
    ++P.References;
    return E;
  }
  return Result{0};
}

llvm::Error KernelFramework::powerPolicyIdle(uint64_t PDO) {
  auto Handle = PnpDeviceHandles.find(PDO);
  if (Handle == PnpDeviceHandles.end())
    return policyError("idle observation requires a framework PDO");
  auto &D = Devices.at(Handle->second);
  if (!D.PowerPolicyOwner || !D.Policy.Started || !D.Policy.Idle ||
      !PowerHost.Now)
    return policyError(
        "idle observation requires a configured active policy owner");
  const auto Previous = D.Policy.IdleSince;
  D.Policy.IdleSince = PowerHost.Now();
  if (auto E = restartIdleTimer(Handle->second)) {
    D.Policy.IdleSince = Previous;
    return E;
  }
  return llvm::Error::success();
}
llvm::Error KernelFramework::powerPolicyActive(uint64_t PDO) {
  auto Handle = PnpDeviceHandles.find(PDO);
  if (Handle == PnpDeviceHandles.end())
    return policyError("activity requires a framework PDO");
  auto &D = Devices.at(Handle->second);
  if (D.Policy.Idle && D.Policy.Idle->systemManaged() && D.Policy.Started)
    if (auto E =
            PowerHost.ManagedIdle(D.Wdm, false, D.Policy.Idle->Timeout100ns))
      return E;
  if (auto E = holdForPoFxComponent(Handle->second))
    return E;
  D.Policy.IdleSince.reset();
  D.Policy.Deadline.reset();
  D.Policy.ManagedPowerNotRequired = false;
  if (D.Policy.Started &&
      (!D.InD0 || D.PowerQueuesHeld || D.Policy.SystemSleeping))
    D.Policy.PowerUpRequested = true;
  return llvm::Error::success();
}
llvm::Error KernelFramework::powerPolicyWake(uint64_t PDO) {
  auto Handle = PnpDeviceHandles.find(PDO);
  if (Handle == PnpDeviceHandles.end())
    return policyError("wake observation requires a framework PDO");
  auto &P = Devices.at(Handle->second).Policy;
  if (P.Armed == KernelPowerPolicy::WakeSource::None || P.WakeTriggered)
    return policyError("wake signal requires one successfully armed source");
  if (!PowerHost.FinishWake)
    return policyError("wake signal lost its provider WAIT_WAKE bridge");
  if (auto E = PowerHost.FinishWake(Devices.at(Handle->second).Wdm, true))
    return E;
  P.WakeTriggered = true;
  P.IdleSince.reset();
  P.PowerUpRequested = true;
  // An Sx signal does not fabricate a system S0 transaction. The explicit
  // system packet remains the authority that makes device power-up eligible.
  return llvm::Error::success();
}
llvm::Error KernelFramework::systemPowerPolicy(uint64_t PDO, bool Sleeping) {
  auto Handle = PnpDeviceHandles.find(PDO);
  if (Handle == PnpDeviceHandles.end())
    return policyError("system transition requires a framework PDO");
  auto &P = Devices.at(Handle->second).Policy;
  P.SystemSleeping = Sleeping;
  P.Deadline.reset();
  return llvm::Error::success();
}
llvm::Error KernelFramework::processPowerPolicy() {
  if (!PowerHost.Now || !PowerHost.Request || PendingCall ||
      !PnpTransitions.empty())
    return llvm::Error::success();
  const uint64_t Now = PowerHost.Now();
  for (auto &[Handle, D] : Devices) {
    auto &P = D.Policy;
    if (P.DevicePowerPending)
      continue;
    if (P.PowerUpRequested && !P.SystemSleeping && !P.Failure) {
      if (!D.InD0) {
        if (auto E = PowerHost.Request(D.Wdm, DevicePowerState::D0,
                                       PowerPolicyHost::RequestMode::Issue))
          return E;
      }
      P.PowerUpRequested = false;
      return llvm::Error::success();
    }
    if (!P.Deadline && P.IdleSince && P.Started && !P.Failure)
      if (auto E = restartIdleTimer(Handle))
        return E;
    const bool ManagedReady =
        P.Idle && P.Idle->systemManaged() && P.ManagedPowerNotRequired;
    if (!ManagedReady && (!P.Deadline || *P.Deadline > Now))
      continue;
    if (!D.InD0 || P.References || P.SystemSleeping || P.Failure ||
        powerPolicyBusy(Handle)) {
      P.Deadline.reset();
      continue;
    }
    if (P.Idle && P.Idle->CanWake &&
        P.Armed == KernelPowerPolicy::WakeSource::None)
      return beginIdlePowerDown(Handle);
    P.IdlePowerDown = true;
    P.ManagedPowerNotRequired = false;
    if (auto E = PowerHost.Request(D.Wdm, DevicePowerState::D3,
                                   PowerPolicyHost::RequestMode::Issue)) {
      P.IdlePowerDown = false;
      return E;
    }
    P.Deadline.reset();
    return llvm::Error::success();
  }
  return llvm::Error::success();
}
std::optional<uint64_t> KernelFramework::nextPowerPolicyTime() const {
  std::optional<uint64_t> Next;
  for (const auto &[Handle, D] : Devices) {
    if (D.Policy.DevicePowerPending)
      continue;
    if (D.Policy.Deadline && (!Next || *D.Policy.Deadline < *Next))
      Next = D.Policy.Deadline;
    if (D.Policy.ManagedPowerNotRequired && D.InD0 && !D.Policy.References &&
        !D.Policy.SystemSleeping && !D.Policy.Failure && PowerHost.Now)
      Next = PowerHost.Now();
    if (D.Policy.PowerUpRequested && !D.Policy.SystemSleeping &&
        !D.Policy.Failure && PnpTransitions.empty() && PowerHost.Now)
      Next = PowerHost.Now();
  }
  return Next;
}
bool KernelFramework::hasPendingPowerPolicy() const {
  return nextPowerPolicyTime().has_value();
}
llvm::Expected<std::optional<uint32_t>>
KernelFramework::powerPolicyWait(uint64_t Device) {
  auto D = Devices.find(Device);
  if (D == Devices.end() || !D->second.Policy.References)
    return policyError("StopIdle wait lost its device or power reference");
  if (D->second.Policy.Failure) {
    --D->second.Policy.References;
    return std::optional<uint32_t>{power_policy::StatusPowerStateInvalid};
  }
  if (D->second.InD0 && !D->second.PowerQueuesHeld)
    return std::optional<uint32_t>{windows::StatusSuccess};
  return std::optional<uint32_t>{};
}
llvm::Expected<uint64_t> KernelFramework::powerPolicyEpoch(uint64_t PDO) const {
  const auto Handle = PnpDeviceHandles.find(PDO);
  if (Handle == PnpDeviceHandles.end())
    return policyError("event lost its framework PDO");
  const auto &P = Devices.at(Handle->second).Policy;
  if (!P.Started)
    return policyError("event requires a successfully started device epoch");
  return P.Epoch;
}

llvm::Expected<bool> KernelFramework::systemSleepNeedsD0(uint64_t PDO) const {
  const auto Handle = PnpDeviceHandles.find(PDO);
  if (Handle == PnpDeviceHandles.end())
    return policyError("system sleep lost its framework PDO");
  const auto &D = Devices.at(Handle->second);
  return !D.InD0 && ((D.Policy.Wake && D.Policy.Wake->Enabled) ||
                     D.Policy.Armed != KernelPowerPolicy::WakeSource::None);
}

llvm::Expected<bool> KernelFramework::systemPowerNeedsD0(uint64_t PDO) const {
  const auto Handle = PnpDeviceHandles.find(PDO);
  if (Handle == PnpDeviceHandles.end())
    return policyError("system wake lost its framework PDO");
  const auto &P = Devices.at(Handle->second).Policy;
  return !P.Idle || !P.Idle->Enabled || P.Idle->CanWake ||
         P.Idle->PowerUpOnSystemWake || P.References || P.PowerUpRequested ||
         P.WakeTriggered;
}

llvm::Error KernelFramework::beginPowerPolicyRequest(uint64_t PDO) {
  auto Handle = PnpDeviceHandles.find(PDO);
  if (Handle == PnpDeviceHandles.end())
    return policyError("power request lost its framework device");
  auto &P = Devices.at(Handle->second).Policy;
  if (P.DevicePowerPending)
    return policyError("another device power request is still pending");
  P.DevicePowerPending = true;
  return llvm::Error::success();
}

llvm::Error KernelFramework::beginIdlePowerDown(uint64_t Device) {
  auto &D = Devices.at(Device);
  if (NextContinuation == UINT64_MAX)
    return policyError("idle callback identity exhausted");
  if (!PowerHost.ArmWake)
    return policyError("idle arm requires the WAIT_WAKE bridge");
  if (auto E = PowerHost.Request(D.Wdm, DevicePowerState::D3,
                                 PowerPolicyHost::RequestMode::Validate))
    return E;
  // Arm before requesting D3. A rejected arm is not a failed SET_POWER IRP:
  // no device-power packet has reached either the framework or the provider.
  const uint64_t Token = NextContinuation++;
  PnpTransition Transition{0, Device};
  Transition.NotificationOnly = true;
  Transition.IdlePolicy = true;
  Transition.Remaining.push_back({PnpPhase::ArmWakeFromS0});
  Continuations.emplace(Token, Continuation{});
  PnpTransitions.emplace(Token, std::move(Transition));
  D.PowerQueuesHeld = true;
  D.Policy.Deadline.reset();
  if (auto E = schedulePnpCallback(Token))
    return E;
  if (PnpTransitions.at(Token).CallbacksComplete) {
    auto Completed = advance(Token);
    if (!Completed)
      return Completed.takeError();
    if (!*Completed)
      return policyError("idle arm did not finish its internal continuation");
    const uint32_t Status = PnpTransitions.at(Token).Status;
    PnpTransitions.erase(Token);
    return finishIdlePowerDown(Device, Status);
  }
  return llvm::Error::success();
}

llvm::Error KernelFramework::finishIdlePowerDown(uint64_t Device,
                                                 uint32_t Status) {
  auto D = Devices.find(Device);
  if (D == Devices.end() || !D->second.InD0)
    return policyError("idle arm lost its D0 device");
  if (Status & profile::NTStatusFailureMask) {
    D->second.Policy.IdleSince.reset();
    D->second.Policy.Deadline.reset();
    D->second.PowerQueuesHeld = false;
    return llvm::Error::success();
  }
  D->second.Policy.IdlePowerDown = true;
  D->second.Policy.ManagedPowerNotRequired = false;
  return PowerHost.Request(D->second.Wdm, DevicePowerState::D3,
                           PowerPolicyHost::RequestMode::Issue);
}

llvm::Error KernelFramework::finishPowerPolicyRequest(uint64_t PDO,
                                                      uint32_t Status,
                                                      bool SetPower) {
  auto Handle = PnpDeviceHandles.find(PDO);
  if (Handle == PnpDeviceHandles.end())
    return policyError("power completion lost its framework policy owner");
  auto &P = Devices.at(Handle->second).Policy;
  P.DevicePowerPending = false;
  if (SetPower && (Status & profile::NTStatusFailureMask) &&
      !(P.IdlePowerDown && Devices.at(Handle->second).InD0))
    P.Failure = Status;
  return llvm::Error::success();
}
llvm::Error KernelFramework::powerPolicyPermission(uint64_t PDO,
                                                   bool NotRequired) {
  const auto Handle = PnpDeviceHandles.find(PDO);
  if (Handle == PnpDeviceHandles.end())
    return policyError("PoFx decision lost its framework device");
  auto &D = Devices.at(Handle->second);
  if (!D.Policy.Idle || !D.Policy.Idle->systemManaged() || !D.Policy.Started)
    return policyError(
        "PoFx decision requires a started system-managed policy");
  if (NotRequired && (!D.Policy.Idle->Enabled || !D.Policy.IdleSince ||
                      D.Policy.References || powerPolicyBusy(Handle->second)))
    return policyError("PoFx power-down decision raced with device activity");
  D.Policy.ManagedPowerNotRequired = NotRequired;
  if (!NotRequired)
    D.Policy.PowerUpRequested = true;
  return llvm::Error::success();
}

llvm::Expected<bool>
KernelFramework::powerPolicyDeviceReady(uint64_t PDO, bool Required) const {
  const auto Handle = PnpDeviceHandles.find(PDO);
  if (Handle == PnpDeviceHandles.end())
    return policyError("PoFx completion lost its framework device");
  const auto &D = Devices.at(Handle->second);
  if (D.Policy.Failure)
    return policyError("PoFx device power transaction failed");
  return D.Policy.Started && !D.Policy.DevicePowerPending &&
         (Required ? D.InD0 : !D.InD0);
}

llvm::Expected<bool> KernelFramework::allowsD3Cold(uint64_t PDO) const {
  const auto Handle = PnpDeviceHandles.find(PDO);
  if (Handle == PnpDeviceHandles.end())
    return false;
  const auto &D = Devices.at(Handle->second);
  if (!D.Policy.Idle || D.Policy.Idle->ExcludeD3Cold == power_policy::True ||
      !PowerHost.ColdAllowed)
    return false;
  return PowerHost.ColdAllowed(
      D.Wdm, D.Policy.Armed != KernelPowerPolicy::WakeSource::None,
      D.Policy.SystemSleeping, D.Policy.Idle->ExcludeD3Cold);
}

} // namespace neverd::emulation
