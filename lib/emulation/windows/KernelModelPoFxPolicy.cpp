//===- KernelModelPoFxPolicy.cpp - Framework-owned PoFx registration -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Connect framework idle policy to component and device power completion.
///
//===----------------------------------------------------------------------===//
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
llvm::Error policyError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "PoFx policy: " + Text);
}
KernelPoFx::Registration
frameworkRegistration(uint64_t PDO,
                      const KernelFrameworkPoFxSettings &Settings) {
  KernelPoFx::Registration Registration;
  Registration.PDO = PDO;
  Registration.Owner = KernelPoFx::RegistrationOwner::Framework;
  Registration.Context = Settings.Context;
  Registration.Routines = Settings.Routines;
  Registration.Components.push_back(Settings.Component);
  return Registration;
}
} // namespace

void KernelModel::configureFrameworkPoFxHost() {
  KernelFramework::PoFxHost Host;
  Host.ReadComponent = [this](uint64_t Address) {
    return readPoFxComponent(Address);
  };
  Host.Validate =
      [this](uint64_t Device,
             const KernelFrameworkPoFxSettings &Settings) -> llvm::Error {
    auto PDO = pnpDeviceForRoute(Device);
    if (!PDO)
      return PDO.takeError();
    const uint64_t Candidate = (NextAllocation + profile::PointerSize - 1) &
                               ~(profile::PointerSize - 1);
    return PoFx.canRegisterDevice(Candidate,
                                  frameworkRegistration(*PDO, Settings));
  };
  Host.Register = [this](uint64_t Device,
                         const KernelFrameworkPoFxSettings &Settings)
      -> llvm::Expected<uint64_t> {
    auto PDO = pnpDeviceForRoute(Device);
    if (!PDO)
      return PDO.takeError();
    if (auto E = validatePoFxRegistrationDevice(*PDO))
      return E;
    auto Registration = frameworkRegistration(*PDO, Settings);
    const uint64_t Candidate = (NextAllocation + profile::PointerSize - 1) &
                               ~(profile::PointerSize - 1);
    if (auto E = PoFx.canRegisterDevice(Candidate, Registration))
      return E;
    auto Handle = allocate(profile::PointerSize, profile::PointerSize);
    if (!Handle)
      return Handle.takeError();
    if (auto E = PoFx.registerDevice(*Handle, std::move(Registration)))
      return E;
    PoFxDeviceObjects.emplace(*Handle, Device);
    return *Handle;
  };
  Host.Start = [this](uint64_t Handle) -> llvm::Error {
    const auto *Registration = PoFx.registration(Handle);
    if (!Registration ||
        Registration->Owner != KernelPoFx::RegistrationOwner::Framework)
      return policyError("framework start requires its live registration");
    // START holds the single framework component active. Policy may release
    // that reference only after the completed START reaches the idle boundary.
    if (auto E = PoFx.activate(Handle, 0))
      return E;
    return PoFx.start(Handle);
  };
  Host.CanUnregister = [this](uint64_t Handle) -> llvm::Expected<bool> {
    const auto *Registration = PoFx.registration(Handle);
    if (!Registration ||
        Registration->Owner != KernelPoFx::RegistrationOwner::Framework)
      return policyError("framework teardown requires its live registration");
    if (FrameworkPoFxPowerWaits.contains(Handle))
      return false;
    return PoFx.callbacksDrained(Handle);
  };
  Host.Quiesce = [this](uint64_t Handle) {
    return PoFx.quiesceFrameworkRegistration(Handle);
  };
  Host.Unregister = [this](uint64_t Handle) -> llvm::Error {
    const auto Owner = PoFxDeviceObjects.find(Handle);
    const auto *Registration = PoFx.registration(Handle);
    if (Owner == PoFxDeviceObjects.end() || !Registration ||
        Registration->Owner != KernelPoFx::RegistrationOwner::Framework)
      return policyError("framework teardown lost its device registration");
    return retireFrameworkPoFx(Owner->second);
  };
  Host.ComponentReady = [this](uint64_t Device) -> llvm::Expected<bool> {
    auto PDO = pnpDeviceForRoute(Device);
    if (!PDO)
      return PDO.takeError();
    auto Handle = PoFx.handleForPDO(*PDO);
    if (!Handle)
      return true;
    if (PoFx.registration(*Handle)->Owner !=
        KernelPoFx::RegistrationOwner::Framework)
      return policyError(
          "framework queue requires its own component registration");
    return PoFx.conditionReached(*Handle, 0, true);
  };
  Framework->setPoFxHost(std::move(Host));
}

llvm::Error KernelModel::setFrameworkPoFxIdle(uint64_t Device, bool Idle,
                                              uint64_t Timeout) {
  if (auto E = PoFx.process(Scheduler.now100ns()))
    return E;
  auto PDO = pnpDeviceForRoute(Device);
  if (!PDO)
    return PDO.takeError();
  auto Handle = PoFx.handleForPDO(*PDO);
  if (!Handle)
    return policyError("system-managed idle requires its START registration");
  if (PoFx.registration(*Handle)->Owner !=
      KernelPoFx::RegistrationOwner::Framework)
    return policyError("system-managed timeout conflicts with driver-owned "
                       "component registration");
  if (auto E = PoFx.setDeviceIdleTimeout(*Handle, Timeout))
    return E;
  auto Component = PoFx.component(*Handle, 0);
  if (!Component)
    return Component.takeError();
  if (Idle && Component->References)
    return PoFx.idle(*Handle, 0);
  if (!Idle && !Component->References)
    return PoFx.activate(*Handle, 0);
  return llvm::Error::success();
}

llvm::Error KernelModel::retireFrameworkPoFx(uint64_t Device) {
  auto PDO = pnpDeviceForRoute(Device);
  if (!PDO)
    return PDO.takeError();
  auto Handle = PoFx.handleForPDO(*PDO);
  if (!Handle)
    return llvm::Error::success();
  if (PoFx.registration(*Handle)->Owner !=
      KernelPoFx::RegistrationOwner::Framework)
    return policyError(
        "framework cleanup cannot unregister a driver-owned handle");
  if (FrameworkPoFxPowerWaits.contains(*Handle))
    return policyError(
        "device removal still owns a power completion acknowledgement");
  if (auto E = PoFx.unregisterDevice(*Handle))
    return E;
  PoFxDeviceObjects.erase(*Handle);
  FreedRanges.emplace(*Handle, profile::PointerSize);
  return llvm::Error::success();
}

llvm::Error
KernelModel::processInternalPoFxCallback(const KernelPoFx::Callback &Call) {
  if (!Framework)
    return policyError("internal callback has no framework owner");
  switch (Call.Kind) {
  case KernelPoFx::CallbackKind::ActiveCondition:
    return llvm::Error::success();
  case KernelPoFx::CallbackKind::IdleCondition:
    return PoFx.completeIdleCondition(Call.Handle, Call.Component);
  case KernelPoFx::CallbackKind::IdleState:
    return PoFx.completeIdleState(Call.Handle, Call.Component);
  case KernelPoFx::CallbackKind::DevicePowerRequired:
  case KernelPoFx::CallbackKind::DevicePowerNotRequired: {
    const bool Required =
        Call.Kind == KernelPoFx::CallbackKind::DevicePowerRequired;
    if (FrameworkPoFxPowerWaits.contains(Call.Handle))
      return policyError("device has overlapping power acknowledgements");
    if (auto E = Framework->powerPolicyPermission(Call.PDO, !Required))
      return E;
    // The typed internal callback returns now. Its acknowledgement and PoFx
    // registration remain owned until the actual device-power IRP completes.
    FrameworkPoFxPowerWaits.emplace(Call.Handle, Required);
    return llvm::Error::success();
  }
  }
  llvm_unreachable("all PoFx callback kinds handled");
}

llvm::Error KernelModel::processPoFxCallbacks() {
  if (auto E = PoFx.process(Scheduler.now100ns()))
    return E;
  for (auto It = FrameworkPoFxPowerWaits.begin();
       It != FrameworkPoFxPowerWaits.end();) {
    const auto *Registration = PoFx.registration(It->first);
    if (!Registration || !Framework)
      return policyError("pending power acknowledgement lost its owner");
    auto Ready =
        Framework->powerPolicyDeviceReady(Registration->PDO, It->second);
    if (!Ready)
      return Ready.takeError();
    if (!*Ready) {
      ++It;
      continue;
    }
    auto E = It->second ? PoFx.reportDevicePoweredOn(It->first)
                        : PoFx.completeDevicePowerNotRequired(It->first);
    if (E)
      return E;
    It = FrameworkPoFxPowerWaits.erase(It);
  }
  if (auto E = queuePoFxCallbacks())
    return E;
  if (Framework) {
    if (auto E = Framework->processPowerPolicy())
      return E;
    if (auto E = Framework->resumePoFxTransitions())
      return E;
    if (auto E = completeFrameworkTransitionIfReady())
      return E;
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
