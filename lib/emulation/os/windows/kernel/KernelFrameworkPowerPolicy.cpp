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

llvm::Error KernelFramework::refreshUsbIdle(uint64_t Device) {
  auto &P = Devices.at(Device).Policy;
  if (!P.UsbIdle)
    return llvm::Error::success();
  if (!PowerHost.HasUsbIdle)
    return policyError("USB idle requires its packet ownership bridge");
  auto Outstanding = PowerHost.HasUsbIdle(P.UsbIdle->Key);
  if (!Outstanding)
    return Outstanding.takeError();
  if (!*Outstanding)
    P.UsbIdle.reset();
  return llvm::Error::success();
}

llvm::Error KernelFramework::completeManagedUsbPowerDown(
    uint64_t Device, PowerPolicyHost::RequestMode Mode) {
  auto &D = Devices.at(Device);
  auto &P = D.Policy;
  if (!P.Idle || !P.Idle->usesUsbIdle() || !P.Idle->systemManaged() ||
      !P.ManagedPowerNotRequired)
    return llvm::Error::success();
  if (!PowerHost.CompletePowerNotRequired)
    return policyError("managed USB requires its PoFx response bridge");
  if (auto E = PowerHost.CompletePowerNotRequired(D.Wdm, Mode))
    return E;
  if (Mode == PowerPolicyHost::RequestMode::Issue)
    P.ManagedPowerNotRequired = false;
  return llvm::Error::success();
}

llvm::Error KernelFramework::restoreManagedUsbActivity(uint64_t Device) {
  auto &D = Devices.at(Device);
  const auto &P = D.Policy;
  if (!P.Idle || !P.Idle->usesUsbIdle() || !P.Idle->systemManaged())
    return llvm::Error::success();
  if (auto E = PowerHost.ManagedIdle(D.Wdm, false, P.Idle->Timeout100ns))
    return E;
  return holdForPoFxComponent(Device);
}

llvm::Error KernelFramework::cancelUsbIdle(uint64_t Device) {
  if (auto E = refreshUsbIdle(Device))
    return E;
  if (auto E = completeManagedUsbPowerDown(
          Device, PowerPolicyHost::RequestMode::Validate))
    return E;
  auto &P = Devices.at(Device).Policy;
  if (P.UsbIdle) {
    if (!PowerHost.CancelUsbIdle)
      return policyError("USB idle cancellation requires its packet bridge");
    const auto Key = P.UsbIdle->Key;
    if (auto E = PowerHost.CancelUsbIdle(
            Key, PowerPolicyHost::RequestMode::Validate))
      return E;
    if (auto E =
            PowerHost.CancelUsbIdle(Key, PowerPolicyHost::RequestMode::Issue))
      return E;
    if (auto E = refreshUsbIdle(Device))
      return E;
  }
  return completeManagedUsbPowerDown(Device,
                                     PowerPolicyHost::RequestMode::Issue);
}

llvm::Error
KernelFramework::requestIdleDevicePower(uint64_t Device,
                                        PowerPolicyHost::RequestMode Mode) {
  const auto &D = Devices.at(Device);
  const auto &P = D.Policy;
  if (P.Idle->usesUsbIdle()) {
    if (!P.UsbIdle || !P.UsbIdle->CallbackToken ||
        !PowerHost.RequestUsbIdlePower)
      return policyError("USB power down requires its entered idle callback");
    return PowerHost.RequestUsbIdlePower(D.Wdm, P.UsbIdle->Key,
                                         *P.UsbIdle->CallbackToken, Mode);
  }
  return PowerHost.Request(D.Wdm, P.Idle->DxState, Mode);
}

llvm::Error KernelFramework::restartIdleTimer(uint64_t Device) {
  auto &D = Devices.at(Device);
  auto &P = D.Policy;
  if (auto E = refreshUsbIdle(Device))
    return E;
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
  if (P.UsbIdle || Active || !CanIdle) {
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
    const bool Usb =
        Idle && Field(policy::IdleCapabilities) == policy::UsbSelectiveSuspend;
    auto DxState = DevicePowerState(
        Field(Idle ? policy::IdleDxState : policy::WakeDxState));
    if (!Usb && (!isSupportedDriverDevicePower(DxState) ||
                 DxState == DevicePowerState::D0))
      return policyError("settings require an explicit supported low-power "
                         "device state");
    if (Idle) {
      if (Field(policy::IdleCapabilities) != policy::CannotWake &&
          Field(policy::IdleCapabilities) != policy::CanWake && !Usb)
        return policyError("unknown idle capability");
      if ((Field(policy::IdleTimeoutType) == policy::DriverManagedTimeout &&
           !Field(policy::IdleTimeout) && !Usb) ||
          Field(policy::IdleTimeoutType) >
              policy::SystemManagedTimeoutWithHint ||
          Field(policy::IdleUserControl) != policy::NoUserControl ||
          Field(policy::IdleExcludeD3Cold) > policy::UseDefault)
        return policyError("idle requires a valid timeout policy and no user "
                           "override");
      const bool Managed =
          Field(policy::IdleTimeoutType) != policy::DriverManagedTimeout;
      if (Usb && Managed && !PowerHost.CompletePowerNotRequired)
        return policyError("managed USB requires its PoFx response bridge");
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
      bool CanWake = Field(policy::IdleCapabilities) == policy::CanWake;
      if (Usb) {
        if (!PowerHost.ResolveUsbIdle || !PowerHost.SubmitUsbIdle ||
            !PowerHost.CancelUsbIdle || !PowerHost.HasUsbIdle ||
            !PowerHost.RequestUsbIdlePower || !PowerHost.AbortUsbIdlePower ||
            !PowerHost.FinishUsbIdleCallback)
          return policyError("USB idle settings require the USB packet bridge");
        if (Field(policy::IdlePowerUpOnSystemWake) != policy::UseDefault)
          return Result{windows::StatusInvalidParameter};
        auto Resolved =
            PowerHost.ResolveUsbIdle(D->second.Wdm, Field(policy::IdleDxState));
        if (!Resolved)
          return Resolved.takeError();
        if (Resolved->DxState != DevicePowerState::D2)
          return policyError("USB idle requires a supported D2 capability");
        DxState = Resolved->DxState;
        CanWake = Resolved->CanWake;
      }
      if (CanWake) {
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
      KernelPowerPolicy::IdleSettings Settings;
      Settings.DxState = DxState;
      Settings.Enabled = Field(policy::IdleEnabled) != policy::False;
      Settings.CanWake = CanWake;
      Settings.PowerUpOnSystemWake =
          Previous ? Previous->PowerUpOnSystemWake
                   : Field(policy::IdlePowerUpOnSystemWake) == policy::True;
      const uint64_t Timeout = !Field(policy::IdleTimeout) && Usb
                                   ? policy::DefaultIdleTimeoutMilliseconds
                                   : Field(policy::IdleTimeout);
      Settings.Timeout100ns =
          Field(policy::IdleTimeoutType) == policy::SystemManagedTimeout
              ? 0
              : Timeout * policy::TicksPerMillisecond;
      Settings.TimeoutType = Field(policy::IdleTimeoutType);
      Settings.ExcludeD3Cold =
          Previous ? Previous->ExcludeD3Cold : Field(policy::IdleExcludeD3Cold);
      Settings.Capability =
          Usb       ? KernelPowerPolicy::IdleCapability::UsbSelectiveSuspend
          : CanWake ? KernelPowerPolicy::IdleCapability::CanWake
                    : KernelPowerPolicy::IdleCapability::CannotWake;
      if (PowerHost.Now &&
          Settings.Timeout100ns > uint64_t(INT64_MAX) - PowerHost.Now())
        return policyError("idle timeout exceeds the virtual clock range");
      if (auto E = refreshUsbIdle(A[1]))
        return E;
      if (P.UsbIdle && P.UsbIdle->CallbackToken)
        return policyError("settings cannot replace an entered USB callback");
      // All configuration checks precede cancellation of the old registration.
      if (auto E = cancelUsbIdle(A[1]))
        return E;
      if (P.UsbIdle)
        return policyError("USB idle packet did not retire after cancellation");
      P.Idle = Settings;
      if (auto E = restartIdleTimer(A[1])) {
        P.Idle = Previous;
        return E;
      }
      if (!P.Idle->Enabled && P.Started && !D->second.InD0)
        P.PowerUpRequested = true;
    } else {
      if (Field(policy::WakeUserControl) != policy::NoUserControl)
        return policyError("wake requires no user override");
      if (Field(policy::WakeEnabled) > policy::UseDefault)
        return Result{windows::StatusInvalidParameter};
      auto ArmChildren = read(A[2] + policy::WakeChildren, sizeof(uint8_t));
      if (!ArmChildren)
        return ArmChildren.takeError();
      auto Propagate = read(A[2] + policy::WakePropagate, sizeof(uint8_t));
      if (!Propagate)
        return Propagate.takeError();
      if ((*ArmChildren || *Propagate) && !PowerHost.Children)
        return policyError("child wake settings require explicit PDO topology");
      if (*Propagate && !PowerHost.CompleteWakes)
        return policyError("child wake propagation requires batch completion");
      if ((*ArmChildren || *Propagate) && !PowerHost.CancelWakes)
        return policyError("child wake settings require batch cancellation");
      if (Field(policy::WakeEnabled) || *ArmChildren) {
        if (!PowerHost.CanWake)
          return policyError(
              "system wake settings require provider capabilities");
        auto Capable = PowerHost.CanWake(D->second.Wdm, true);
        if (!Capable)
          return Capable.takeError();
        if (!*Capable)
          return Result{policy::StatusPowerStateInvalid};
      }
      P.Wake = KernelPowerPolicy::WakeSettings{
          DxState, Field(policy::WakeEnabled) != 0, *ArmChildren != 0,
          *Propagate != 0};
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
    if (auto E = cancelUsbIdle(A[1]))
      return E;
    if (P.Idle && P.Idle->systemManaged())
      if (auto E =
              PowerHost.ManagedIdle(D->second.Wdm, false, P.Idle->Timeout100ns))
        return E;
    if (auto E = holdForPoFxComponent(A[1]))
      return E;
    ++P.References;
    P.Deadline.reset();
    P.ManagedPowerNotRequired = false;
    if (D->second.InD0 && !D->second.queuesHeld() && !P.DevicePowerPending &&
        !P.UsbIdle)
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
  if (auto E = cancelUsbIdle(Handle->second))
    return E;
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
      (!D.InD0 || D.queuesHeld() || D.Policy.SystemSleeping ||
       D.Policy.DevicePowerPending || D.Policy.UsbIdle))
    D.Policy.PowerUpRequested = true;
  return llvm::Error::success();
}
llvm::Error KernelFramework::systemPowerPolicy(uint64_t PDO, bool Sleeping) {
  auto Handle = PnpDeviceHandles.find(PDO);
  if (Handle == PnpDeviceHandles.end())
    return policyError("system transition requires a framework PDO");
  if (Sleeping)
    if (auto E = cancelUsbIdle(Handle->second))
      return E;
  auto &P = Devices.at(Handle->second).Policy;
  P.SystemSleeping = Sleeping;
  P.Deadline.reset();
  return llvm::Error::success();
}
llvm::Error
KernelFramework::canBeginUsbIdlePermission(UsbIdleKey Key, uint64_t PolicyEpoch,
                                           uint64_t CallbackToken) const {
  const auto Handle = PnpDeviceHandles.find(Key.PDO);
  if (Handle == PnpDeviceHandles.end())
    return policyError("USB permission lost its framework device");
  const auto &D = Devices.at(Handle->second);
  const auto &P = D.Policy;
  if (!CallbackToken || !P.Started || P.Epoch != PolicyEpoch || !P.UsbIdle ||
      P.UsbIdle->Key != Key || P.UsbIdle->CallbackToken || !P.Idle ||
      !P.Idle->usesUsbIdle() || !P.Idle->Enabled || !P.IdleSince ||
      P.References || P.SystemSleeping || P.Failure || P.DevicePowerPending ||
      !D.InD0 || D.PowerQueuesHeld || powerPolicyBusy(Handle->second))
    return policyError("USB permission requires its current idle D0 policy");
  if (PendingCall || !PnpTransitions.empty())
    return policyError("USB permission must wait for framework callbacks");
  if (!PowerHost.RequestUsbIdlePower || !PowerHost.AbortUsbIdlePower ||
      !PowerHost.FinishUsbIdleCallback)
    return policyError("USB permission requires its device-power bridge");
  if (P.Idle->CanWake && P.Armed == KernelPowerPolicy::WakeSource::None) {
    if (NextContinuation == UINT64_MAX)
      return policyError("idle callback identity exhausted");
    if (!PowerHost.ArmWake)
      return policyError("idle arm requires the WAIT_WAKE bridge");
  }
  return PowerHost.RequestUsbIdlePower(D.Wdm, Key, CallbackToken,
                                       PowerPolicyHost::RequestMode::Validate);
}

llvm::Error KernelFramework::beginUsbIdlePermission(UsbIdleKey Key,
                                                    uint64_t PolicyEpoch,
                                                    uint64_t CallbackToken) {
  if (auto E = canBeginUsbIdlePermission(Key, PolicyEpoch, CallbackToken))
    return E;
  const auto Device = PnpDeviceHandles.at(Key.PDO);
  auto &D = Devices.at(Device);
  auto &P = D.Policy;
  P.UsbIdle->CallbackToken = CallbackToken;
  P.Deadline.reset();
  if (P.Idle->CanWake && P.Armed == KernelPowerPolicy::WakeSource::None)
    return beginIdlePowerDown(Device);
  P.IdlePowerDown = true;
  P.ManagedPowerNotRequired = false;
  if (auto E =
          requestIdleDevicePower(Device, PowerPolicyHost::RequestMode::Issue)) {
    P.IdlePowerDown = false;
    return E;
  }
  return llvm::Error::success();
}

llvm::Error KernelFramework::retireUsbIdleRegistration(UsbIdleKey Key) {
  const auto Handle = PnpDeviceHandles.find(Key.PDO);
  if (Handle == PnpDeviceHandles.end())
    return policyError("USB retirement lost its framework device");
  auto &P = Devices.at(Handle->second).Policy;
  if (!P.UsbIdle || P.UsbIdle->Key != Key)
    return policyError("USB retirement requires its exact registration");
  P.UsbIdle.reset();
  return llvm::Error::success();
}

llvm::Error KernelFramework::finishUsbIdlePowerAdmissionFailure(
    UsbIdleKey Key, uint64_t CallbackToken, uint32_t Status) {
  const auto Handle = PnpDeviceHandles.find(Key.PDO);
  if (Handle == PnpDeviceHandles.end())
    return policyError("USB admission failure lost its framework device");
  auto &D = Devices.at(Handle->second);
  auto &P = D.Policy;
  if (Status != windows::StatusInsufficientResources || !P.UsbIdle ||
      P.UsbIdle->Key != Key || P.UsbIdle->CallbackToken != CallbackToken ||
      P.UsbIdle->PowerAdmissionFailed || !D.InD0 || P.DevicePowerPending ||
      P.Armed == KernelPowerPolicy::WakeSource::Sx ||
      !PowerHost.FinishUsbIdleCallback || PendingCall ||
      !PnpTransitions.empty())
    return policyError(
        "USB allocation failure requires its active D0 callback");
  const bool Disarm = P.Armed == KernelPowerPolicy::WakeSource::S0;
  if (Disarm && NextContinuation == UINT64_MAX)
    return policyError("USB cleanup callback identity exhausted");
  P.UsbIdle->PowerAdmissionFailed = true;
  P.IdleSince.reset();
  P.Deadline.reset();
  P.IdlePowerDown = false;
  P.ManagedPowerNotRequired = false;
  if (!Disarm)
    return finishIdlePowerDown(Handle->second, Status);
  const uint64_t Token = NextContinuation++;
  PnpTransition Transition{0, Handle->second};
  Transition.NotificationOnly = true;
  Transition.IdlePolicy = true;
  Transition.Status = Status;
  Transition.Remaining.push_back({PnpPhase::DisarmWakeFromS0});
  Continuations.emplace(Token, Continuation{});
  PnpTransitions.emplace(Token, std::move(Transition));
  D.PowerQueuesHeld = true;
  if (auto E = schedulePnpCallback(Token))
    return E;
  if (PnpTransitions.at(Token).CallbacksComplete) {
    auto Completed = advance(Token);
    if (!Completed)
      return Completed.takeError();
    if (!*Completed)
      return policyError("USB cleanup did not finish its continuation");
    PnpTransitions.erase(Token);
    return finishIdlePowerDown(Handle->second, Status);
  }
  return llvm::Error::success();
}

llvm::Error KernelFramework::processPowerPolicy() {
  if (!PowerHost.Now || !PowerHost.Request || PendingCall ||
      !PnpTransitions.empty())
    return llvm::Error::success();
  const uint64_t Now = PowerHost.Now();
  for (auto &[Handle, D] : Devices) {
    auto &P = D.Policy;
    if (auto E = refreshUsbIdle(Handle))
      return E;
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
    if (P.UsbIdle)
      continue;
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
    if (P.Idle && P.Idle->usesUsbIdle()) {
      if (auto E = completeManagedUsbPowerDown(
              Handle, PowerPolicyHost::RequestMode::Validate))
        return E;
      auto Key = PowerHost.SubmitUsbIdle(D.Wdm, P.Epoch);
      if (!Key)
        return Key.takeError();
      P.UsbIdle = KernelPowerPolicy::UsbIdleState{*Key, std::nullopt};
      if (auto E = completeManagedUsbPowerDown(
              Handle, PowerPolicyHost::RequestMode::Issue))
        return E;
      P.Deadline.reset();
      P.ManagedPowerNotRequired = false;
      return llvm::Error::success();
    }
    if (P.Idle && P.Idle->CanWake &&
        P.Armed == KernelPowerPolicy::WakeSource::None)
      return beginIdlePowerDown(Handle);
    P.IdlePowerDown = true;
    P.ManagedPowerNotRequired = false;
    if (auto E = PowerHost.Request(D.Wdm, P.Idle->DxState,
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
  if (auto E = refreshUsbIdle(Device))
    return E;
  if (D->second.InD0 && !D->second.queuesHeld() &&
      !D->second.Policy.DevicePowerPending && !D->second.Policy.UsbIdle)
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

llvm::Expected<DevicePowerState>
KernelFramework::systemSleepTarget(uint64_t PDO) const {
  const auto Handle = PnpDeviceHandles.find(PDO);
  if (Handle == PnpDeviceHandles.end())
    return policyError("system sleep lost its framework PDO");
  const auto &P = Devices.at(Handle->second).Policy;
  if (!P.Wake)
    return DevicePowerState::D3;
  if (P.Wake->Enabled)
    return P.Wake->DxState;
  if (P.Wake->ArmForChildren) {
    auto Children = armedWakeChildren(Handle->second);
    if (!Children)
      return Children.takeError();
    if (!Children->empty())
      return P.Wake->DxState;
  }
  return DevicePowerState::D3;
}

llvm::Expected<bool> KernelFramework::systemSleepNeedsD0(uint64_t PDO) const {
  const auto Handle = PnpDeviceHandles.find(PDO);
  if (Handle == PnpDeviceHandles.end())
    return policyError("system sleep lost its framework PDO");
  const auto &D = Devices.at(Handle->second);
  auto Children = armedWakeChildren(Handle->second);
  if (!Children)
    return Children.takeError();
  return !D.InD0 && ((D.Policy.Wake && (D.Policy.Wake->Enabled ||
                                        (D.Policy.Wake->ArmForChildren &&
                                         !Children->empty()))) ||
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
  if (auto E = requestIdleDevicePower(Device,
                                      PowerPolicyHost::RequestMode::Validate))
    return E;
  // Arm before requesting Dx. A rejected arm is not a failed SET_POWER IRP:
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
  if (D->second.Policy.UsbIdle &&
      D->second.Policy.UsbIdle->PowerAdmissionFailed) {
    const auto Usb = *D->second.Policy.UsbIdle;
    if (Status != windows::StatusInsufficientResources || !Usb.CallbackToken ||
        D->second.Policy.Armed != KernelPowerPolicy::WakeSource::None)
      return policyError("USB allocation cleanup lost its failure or disarm");
    if (auto E = PowerHost.FinishUsbIdleCallback(Usb.Key, *Usb.CallbackToken))
      return E;
    if (auto E = refreshUsbIdle(Device))
      return E;
    if (D->second.Policy.UsbIdle)
      return policyError("USB allocation cleanup did not retire its packet");
    D->second.PowerQueuesHeld = false;
    if (auto E = restoreManagedUsbActivity(Device))
      return E;
    std::vector<Step> Presentations;
    if (!D->second.queuesHeld())
      appendPowerQueuePresentations(Device, Presentations);
    if (!Presentations.empty()) {
      auto Started = start(std::move(Presentations));
      if (!Started)
        return Started.takeError();
    }
    return llvm::Error::success();
  }
  if (Status & profile::NTStatusFailureMask) {
    const auto &Usb = D->second.Policy.UsbIdle;
    if (Usb && Usb->CallbackToken) {
      if (auto E = PowerHost.AbortUsbIdlePower(Usb->Key, *Usb->CallbackToken,
                                               Status))
        return E;
      if (auto E = refreshUsbIdle(Device))
        return E;
    }
    D->second.Policy.IdleSince.reset();
    D->second.Policy.Deadline.reset();
    D->second.PowerQueuesHeld = false;
    return restoreManagedUsbActivity(Device);
  }
  D->second.Policy.IdlePowerDown = true;
  D->second.Policy.ManagedPowerNotRequired = false;
  return requestIdleDevicePower(Device, PowerPolicyHost::RequestMode::Issue);
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
  if (SetPower && !(Status & profile::NTStatusFailureMask) &&
      Devices.at(Handle->second).InD0 && !P.SystemSleeping && !P.UsbIdle)
    return restoreManagedUsbActivity(Handle->second);
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
                      D.Policy.References || powerPolicyBusy(Handle->second))) {
    if (!D.Policy.Idle->usesUsbIdle())
      return policyError("PoFx power-down decision raced with device activity");
    if (!PowerHost.CompletePowerNotRequired)
      return policyError("managed USB requires its PoFx response bridge");
    if (auto E = PowerHost.CompletePowerNotRequired(
            D.Wdm, PowerPolicyHost::RequestMode::Validate))
      return E;
    return PowerHost.CompletePowerNotRequired(
        D.Wdm, PowerPolicyHost::RequestMode::Issue);
  }
  if (!NotRequired)
    if (auto E = cancelUsbIdle(Handle->second))
      return E;
  D.Policy.ManagedPowerNotRequired = NotRequired;
  if (!NotRequired)
    D.Policy.PowerUpRequested = true;
  return llvm::Error::success();
}

llvm::Expected<std::optional<uint32_t>>
KernelFramework::powerPolicyDeviceCompletion(uint64_t PDO,
                                             bool Required) const {
  const auto Handle = PnpDeviceHandles.find(PDO);
  if (Handle == PnpDeviceHandles.end())
    return policyError("PoFx completion lost its framework device");
  const auto &D = Devices.at(Handle->second);
  if (D.Policy.DevicePowerPending) {
    // A failed entry must settle Required before its cleanup can unregister
    // PoFx. The real guest failure is terminal even though the power IRP still
    // owns the remaining hardware cleanup. Successful entry must wait for it.
    if (Required)
      for (const auto &[Token, Transition] : PnpTransitions)
        if (Transition.Device == Handle->second && Transition.Entering &&
            Transition.Current.Phase == PnpPhase::PoFxQuiesce &&
            (Transition.Status & profile::NTStatusFailureMask))
          return std::optional<uint32_t>{Transition.Status};
    return std::optional<uint32_t>{};
  }
  if (D.Policy.Failure)
    return D.Policy.Failure;
  if (!Required && D.Policy.Idle && D.Policy.Idle->usesUsbIdle())
    return std::optional<uint32_t>{};
  const bool Ready =
      D.Policy.Started && (Required ? D.InD0 && !D.Policy.UsbIdle : !D.InD0);
  return Ready ? std::optional<uint32_t>{windows::StatusSuccess}
               : std::optional<uint32_t>{};
}

llvm::Expected<bool>
KernelFramework::powerPolicyDeviceReady(uint64_t PDO, bool Required) const {
  auto Completion = powerPolicyDeviceCompletion(PDO, Required);
  if (!Completion)
    return Completion.takeError();
  if (*Completion && (**Completion & profile::NTStatusFailureMask))
    return policyError("PoFx device power transaction failed");
  return Completion->has_value();
}

llvm::Expected<bool> KernelFramework::allowsD3Cold(uint64_t PDO) const {
  const auto Handle = PnpDeviceHandles.find(PDO);
  if (Handle == PnpDeviceHandles.end())
    return false;
  const auto &D = Devices.at(Handle->second);
  if (!D.Policy.Idle || D.Policy.Idle->DxState != DevicePowerState::D3 ||
      D.Policy.Idle->ExcludeD3Cold == power_policy::True ||
      !PowerHost.ColdAllowed)
    return false;
  return PowerHost.ColdAllowed(
      D.Wdm, D.Policy.Armed != KernelPowerPolicy::WakeSource::None,
      D.Policy.SystemSleeping, D.Policy.Idle->ExcludeD3Cold);
}

} // namespace neverd::emulation
