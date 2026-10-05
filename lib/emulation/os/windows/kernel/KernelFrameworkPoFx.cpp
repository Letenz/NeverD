//===- KernelFrameworkPoFx.cpp - Single-component Fx lifecycle ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Read copied KMDF settings and preserve registration and component ownership.
///
//===----------------------------------------------------------------------===//
#include "KernelFramework.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
llvm::Error frameworkPoFxError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "KMDF PoFx: " + Text);
}
} // namespace

llvm::Expected<uint64_t>
KernelFramework::callPoFxSettings(llvm::StringRef, Binding &B,
                                  llvm::ArrayRef<uint64_t> A, uint8_t IRQL) {
  using namespace framework;
  if (IRQL)
    return frameworkPoFxError("settings require PASSIVE_LEVEL");
  auto Object = Objects.find(A[1]);
  auto Device = Devices.find(A[1]);
  if (Object == Objects.end() || Object->second.Binding != B.Globals ||
      Object->second.Deleting || Device == Devices.end() || !Device->second.PDO)
    return frameworkPoFxError("settings require the binding's live PnP device");
  auto &D = Device->second;
  if (!D.PowerPolicyOwner)
    return QueueInvalidDeviceRequest;
  if (!D.Policy.Idle || !D.Policy.Idle->systemManaged())
    return frameworkPoFxError(
        "settings require previously assigned managed idle policy");
  if (D.PoFxSettings)
    return frameworkPoFxError("settings may be assigned only once per device");
  if (D.Policy.Epoch || D.Policy.Started || D.PoFxHandle)
    return frameworkPoFxError(
        "settings must precede completion of the first START");
  if (!PowerFrameworkHost.ReadComponent || !PowerFrameworkHost.Validate ||
      !PowerFrameworkHost.Register || !PowerFrameworkHost.Start ||
      !PowerFrameworkHost.Quiesce || !PowerFrameworkHost.CanUnregister ||
      !PowerFrameworkHost.Unregister || !PowerFrameworkHost.ComponentReady)
    return frameworkPoFxError("custom settings require the complete PoFx host");
  if (auto E = ValidateAccess(A[2], PoFxSettingsSize, false))
    return E;
  auto Size = read(A[2], sizeof(uint32_t));
  if (!Size)
    return Size.takeError();
  if (*Size != PoFxSettingsSize)
    return InfoLengthMismatch;
  KernelFrameworkPoFxSettings Settings;
  uint64_t ComponentAddress = 0, Flags = 0;
  struct Field {
    uint64_t Offset;
    uint64_t *Value;
  };
  const Field Fields[] = {
      {PoFxSettingsPostRegister, &Settings.PostRegister},
      {PoFxSettingsPreUnregister, &Settings.PreUnregister},
      {PoFxSettingsComponent, &ComponentAddress},
      {PoFxSettingsActiveCondition, &Settings.Routines.ActiveCondition},
      {PoFxSettingsIdleCondition, &Settings.Routines.IdleCondition},
      {PoFxSettingsIdleState, &Settings.Routines.IdleState},
      {PoFxSettingsPowerControl, &Settings.Routines.PowerControl},
      {PoFxSettingsContext, &Settings.Context},
      {PoFxSettingsFlags, &Flags}};
  for (const auto &Field : Fields) {
    auto Value = read(A[2] + Field.Offset);
    if (!Value)
      return Value.takeError();
    *Field.Value = *Value;
  }
  auto Directed = read(A[2] + PoFxSettingsDirected, sizeof(uint32_t));
  if (!Directed)
    return Directed.takeError();
  if (*Directed > power_policy::UseDefault)
    return windows::StatusInvalidParameter;
  if (*Directed != power_policy::False || Flags)
    return frameworkPoFxError(
        "directed power and power-relation flags require an explicit provider");
  if (Settings.Routines.PowerControl)
    return frameworkPoFxError(
        "power-control callbacks require an explicit PEP provider");
  if (ComponentAddress) {
    auto Component = PowerFrameworkHost.ReadComponent(ComponentAddress);
    if (!Component)
      return Component.takeError();
    Settings.Component = std::move(*Component);
  }
  if (auto E = PowerFrameworkHost.Validate(D.Wdm, Settings))
    return E;
  D.PoFxSettings = std::move(Settings);
  return windows::StatusSuccess;
}

llvm::Expected<bool> KernelFramework::advancePoFxLifecycle(uint64_t Token) {
  auto &Transition = PnpTransitions.at(Token);
  auto &D = Devices.at(Transition.Device);
  switch (Transition.Current.Phase) {
  case PnpPhase::PoFxRegister: {
    if (!D.Policy.Idle || !D.Policy.Idle->systemManaged() || D.PoFxHandle)
      return false;
    // Default F0 policy hosts may own registration through ManagedIdle.
    // Custom settings require the explicit lifetime bridge validated above.
    if (!PowerFrameworkHost.Register && !D.PoFxSettings)
      return false;
    if (!PowerFrameworkHost.Register)
      return frameworkPoFxError("registration lost its PoFx host");
    const KernelFrameworkPoFxSettings Defaults;
    const auto &Settings = D.PoFxSettings ? *D.PoFxSettings : Defaults;
    auto Handle = PowerFrameworkHost.Register(D.Wdm, Settings);
    if (!Handle)
      return Handle.takeError();
    D.PoFxHandle = *Handle;
    if (!Settings.PostRegister)
      return false;
    PendingCall = GuestCall{
        Token, Settings.PostRegister, {Transition.Device, D.PoFxHandle}};
    return true;
  }
  case PnpPhase::PoFxStart:
    if (!D.PoFxHandle || D.PoFxStarted)
      return false;
    if (!PowerFrameworkHost.Start)
      return frameworkPoFxError("registration start lost its PoFx host");
    if (auto E = PowerFrameworkHost.Start(D.PoFxHandle))
      return E;
    D.PoFxStarted = true;
    return false;
  case PnpPhase::PoFxQuiesce:
  case PnpPhase::PoFxUnregister: {
    if (!D.PoFxHandle)
      return false;
    const bool Quiescing = Transition.Current.Phase == PnpPhase::PoFxQuiesce;
    if (Quiescing) {
      if (!PowerFrameworkHost.Quiesce)
        return frameworkPoFxError("quiescing lost its PoFx host");
      if (auto E = PowerFrameworkHost.Quiesce(D.PoFxHandle))
        return E;
    }
    auto Ready = canUnregisterPoFx(Transition.Device);
    if (!Ready)
      return Ready.takeError();
    if (!*Ready) {
      Transition.Remaining.push_front(Transition.Current);
      Transition.WaitingForPoFx = true;
      return true;
    }
    if (Quiescing)
      return false;
    if (D.PoFxSettings && D.PoFxSettings->PreUnregister) {
      PendingCall = GuestCall{Token,
                              D.PoFxSettings->PreUnregister,
                              {Transition.Device, D.PoFxHandle}};
      return true;
    }
    if (auto E = unregisterPoFx(Transition.Device))
      return E;
    return false;
  }
  default:
    llvm_unreachable("only PoFx lifecycle phases enter this helper");
  }
}

llvm::Expected<bool> KernelFramework::canUnregisterPoFx(uint64_t Device) const {
  const auto &D = Devices.at(Device);
  if (!D.PoFxHandle)
    return true;
  if (!PowerFrameworkHost.CanUnregister)
    return frameworkPoFxError("unregistration lost its PoFx host");
  return PowerFrameworkHost.CanUnregister(D.PoFxHandle);
}

llvm::Error KernelFramework::unregisterPoFx(uint64_t Device) {
  auto &D = Devices.at(Device);
  if (!D.PoFxHandle || !PowerFrameworkHost.Unregister)
    return frameworkPoFxError("unregistration lost its live handle or host");
  if (auto E = PowerFrameworkHost.Unregister(D.PoFxHandle))
    return E;
  D.PoFxHandle = 0;
  D.PoFxStarted = false;
  D.PoFxComponentHeld = false;
  return llvm::Error::success();
}

llvm::Error KernelFramework::holdForPoFxComponent(uint64_t Device) {
  auto &D = Devices.at(Device);
  if (!D.PoFxHandle || !PowerFrameworkHost.ComponentReady)
    return llvm::Error::success();
  auto Ready = PowerFrameworkHost.ComponentReady(D.Wdm);
  if (!Ready)
    return Ready.takeError();
  D.PoFxComponentHeld = !*Ready;
  return llvm::Error::success();
}

llvm::Error KernelFramework::resumePoFxTransitions() {
  if (auto E = resumePausedPnp())
    return E;
  if (CallbackIRQL || PendingCall)
    return llvm::Error::success();
  std::vector<Step> Steps;
  std::vector<uint64_t> ReadyDevices;
  for (auto &[Handle, D] : Devices) {
    if (!D.PoFxHandle || !D.PoFxStarted || !D.InD0 || !D.Policy.Started ||
        !D.PoFxComponentHeld || D.PowerQueuesHeld ||
        D.Policy.DevicePowerPending || !PowerFrameworkHost.ComponentReady ||
        std::any_of(
            PnpTransitions.begin(), PnpTransitions.end(),
            [&](const auto &Entry) { return Entry.second.Device == Handle; }))
      continue;
    auto Ready = PowerFrameworkHost.ComponentReady(D.Wdm);
    if (!Ready)
      return Ready.takeError();
    if (!*Ready)
      continue;
    ReadyDevices.push_back(Handle);
    appendPowerQueuePresentations(Handle, Steps);
  }
  if (!Steps.empty())
    if (auto E = preflightCancellationToken(0))
      return E;
  for (const uint64_t Handle : ReadyDevices)
    Devices.at(Handle).PoFxComponentHeld = false;
  if (Steps.empty())
    return flushReadyNotifications();
  auto Started = start(std::move(Steps));
  if (!Started)
    return Started.takeError();
  return flushReadyNotifications();
}
} // namespace neverd::emulation
