//===- KernelFrameworkPowerPolicyCalls.cpp - Power policy calls ----===//
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
using namespace framework;
namespace policy = power_policy;
llvm::Error policyError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "KMDF power policy: " + Text);
}
} // namespace

llvm::Expected<KernelFramework::Device *>
KernelFramework::powerPolicyDeviceForCall(Binding &B, uint64_t Handle) {
  auto D = Devices.find(Handle);
  auto O = Objects.find(Handle);
  if (D == Devices.end() || O == Objects.end() || O->second.Deleting ||
      O->second.Binding != B.Globals || !D->second.PDO)
    return policyError("operation requires the binding's live PnP device");
  return &D->second;
}

llvm::Expected<uint64_t> KernelFramework::callPowerPolicyRegister(
    llvm::StringRef, Binding &B, llvm::ArrayRef<uint64_t> A, uint8_t IRQL) {
  if (IRQL)
    return policyError(
        "operation requires its documented passive/dispatch IRQL");
  auto Init = DeviceInits.find(A[1]);
  if (Init == DeviceInits.end() || Init->second.Binding != B.Globals ||
      Init->second.Kind != DeviceInitKind::Pnp)
    return policyError("callbacks require the binding's live PnP initializer");
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
  return 0;
}

llvm::Expected<uint64_t>
KernelFramework::callPowerPolicySettings(llvm::StringRef Name, Binding &B,
                                         llvm::ArrayRef<uint64_t> A,
                                         uint8_t IRQL) {
  if (IRQL > scheduler::DispatchLevel)
    return policyError(
        "operation requires its documented passive/dispatch IRQL");
  auto Selected = powerPolicyDeviceForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &D = **Selected;
  if (!D.PowerPolicyOwner)
    return ControlInvalidDeviceRequest;
  auto &P = D.Policy;
  const bool Idle = Name == api::WdfDeviceAssignS0IdleSettings;
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
    return InfoLengthMismatch;
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
  auto DxState =
      DevicePowerState(Field(Idle ? policy::IdleDxState : policy::WakeDxState));
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
        Field(policy::IdleTimeoutType) > policy::SystemManagedTimeoutWithHint ||
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
    if (Managed && !P.Idle && (P.Started || D.InD0))
      return policyError(
          "system-managed policy must precede first D0 entry completion");
    if (Field(policy::IdleEnabled) > policy::UseDefault ||
        Field(policy::IdlePowerUpOnSystemWake) > policy::UseDefault)
      return windows::StatusInvalidParameter;
    bool CanWake = Field(policy::IdleCapabilities) == policy::CanWake;
    if (Usb) {
      if (!PowerHost.ResolveUsbIdle || !PowerHost.SubmitUsbIdle ||
          !PowerHost.CancelUsbIdle || !PowerHost.HasUsbIdle ||
          !PowerHost.RequestUsbIdlePower || !PowerHost.AbortUsbIdlePower ||
          !PowerHost.FinishUsbIdleCallback)
        return policyError("USB idle settings require the USB packet bridge");
      if (Field(policy::IdlePowerUpOnSystemWake) != policy::UseDefault)
        return windows::StatusInvalidParameter;
      auto Resolved =
          PowerHost.ResolveUsbIdle(D.Wdm, Field(policy::IdleDxState));
      if (!Resolved)
        return Resolved.takeError();
      if (Resolved->DxState != DevicePowerState::D2)
        return policyError("USB idle requires a supported D2 capability");
      DxState = Resolved->DxState;
      CanWake = Resolved->CanWake;
    }
    if (CanWake) {
      if (!PowerHost.CanWake)
        return policyError("idle wake settings require provider capabilities");
      auto Capable = PowerHost.CanWake(D.Wdm, false);
      if (!Capable)
        return Capable.takeError();
      if (!*Capable)
        return policy::StatusPowerStateInvalid;
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
    if (!P.Idle->Enabled && P.Started && !D.InD0)
      P.PowerUpRequested = true;
  } else {
    if (Field(policy::WakeUserControl) != policy::NoUserControl)
      return policyError("wake requires no user override");
    if (Field(policy::WakeEnabled) > policy::UseDefault)
      return windows::StatusInvalidParameter;
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
      auto Capable = PowerHost.CanWake(D.Wdm, true);
      if (!Capable)
        return Capable.takeError();
      if (!*Capable)
        return policy::StatusPowerStateInvalid;
    }
    P.Wake = KernelPowerPolicy::WakeSettings{
        DxState, Field(policy::WakeEnabled) != 0, *ArmChildren != 0,
        *Propagate != 0};
  }
  return 0;
}

llvm::Expected<uint64_t> KernelFramework::callPowerPolicyStopIdle(
    llvm::StringRef, Binding &B, llvm::ArrayRef<uint64_t> A, uint8_t IRQL) {
  if (IRQL > scheduler::DispatchLevel || (uint8_t(A[2]) && IRQL))
    return policyError(
        "operation requires its documented passive/dispatch IRQL");
  auto Selected = powerPolicyDeviceForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &D = **Selected;
  if (!D.PowerPolicyOwner)
    return policy::StatusInvalidDeviceState;
  auto &P = D.Policy;
  if (P.Failure)
    return policy::StatusPowerStateInvalid;
  if (!P.Started && !D.InD0)
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
    if (auto E = PowerHost.ManagedIdle(D.Wdm, false, P.Idle->Timeout100ns))
      return E;
  if (auto E = holdForPoFxComponent(A[1]))
    return E;
  ++P.References;
  P.Deadline.reset();
  P.ManagedPowerNotRequired = false;
  if (D.InD0 && !D.queuesHeld() && !P.DevicePowerPending && !P.UsbIdle)
    return windows::StatusSuccess;
  P.PowerUpRequested = true;
  return windows::StatusPending;
}

llvm::Expected<uint64_t> KernelFramework::callPowerPolicyResumeIdle(
    llvm::StringRef, Binding &B, llvm::ArrayRef<uint64_t> A, uint8_t IRQL) {
  if (IRQL > scheduler::DispatchLevel)
    return policyError(
        "operation requires its documented passive/dispatch IRQL");
  auto Selected = powerPolicyDeviceForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &D = **Selected;
  if (!D.PowerPolicyOwner)
    return policyError("ResumeIdle requires power-policy ownership");
  auto &P = D.Policy;
  if (!P.References)
    return policyError("ResumeIdle has no matching successful StopIdle");
  --P.References;
  if (auto E = restartIdleTimer(A[1])) {
    ++P.References;
    return E;
  }
  return 0;
}

} // namespace neverd::emulation
