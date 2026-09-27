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

#include "llvm/ADT/StringExtras.h"

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
  Host.Quiesce = [this](uint64_t Handle) -> llvm::Error {
    const auto Wait = FrameworkPoFxPowerWaits.find(Handle);
    const auto *Registration = PoFx.registration(Handle);
    if (Wait == FrameworkPoFxPowerWaits.end() || !Registration ||
        Wait->second.Kind != KernelPoFx::CallbackKind::DevicePowerRequired)
      return PoFx.quiesceFrameworkRegistration(Handle);
    auto Completion =
        Framework->powerPolicyDeviceCompletion(Registration->PDO, true);
    if (!Completion)
      return Completion.takeError();
    if (!*Completion || !(**Completion & profile::NTStatusFailureMask))
      return PoFx.quiesceFrameworkRegistration(Handle);
    // Acknowledge the failed entry and suppress future component callbacks as
    // one transaction. The power IRP retains its failure and cleanup owner.
    if (auto E = PoFx.quiesceFrameworkRegistration(Handle,
                                                   Wait->second.CallbackToken))
      return E;
    FrameworkPoFxPowerWaits.erase(Wait);
    return llvm::Error::success();
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

llvm::Error KernelModel::completeFrameworkPowerNotRequired(
    uint64_t Device, KernelFramework::PowerPolicyHost::RequestMode Mode) {
  auto PDO = pnpDeviceForRoute(Device);
  if (!PDO)
    return PDO.takeError();
  const auto Handle = PoFx.handleForPDO(*PDO);
  if (!Handle)
    return policyError("USB idle response requires a live PoFx registration");
  const auto Owner = PoFxDeviceObjects.find(*Handle);
  const auto Wait = FrameworkPoFxPowerWaits.find(*Handle);
  const auto *Registration = PoFx.registration(*Handle);
  if (Owner == PoFxDeviceObjects.end() || Owner->second != Device ||
      !Registration ||
      Registration->Owner != KernelPoFx::RegistrationOwner::Framework ||
      Wait == FrameworkPoFxPowerWaits.end() ||
      Wait->second.Kind != KernelPoFx::CallbackKind::DevicePowerNotRequired)
    return policyError("USB idle response lost its framework power callback");
  if (auto E = PoFx.canCompleteDevicePowerNotRequired(
          *Handle, Wait->second.CallbackToken))
    return E;
  if (Mode == KernelFramework::PowerPolicyHost::RequestMode::Validate)
    return llvm::Error::success();
  if (auto E = PoFx.completeDevicePowerNotRequired(*Handle))
    return E;
  FrameworkPoFxPowerWaits.erase(Wait);
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
    FrameworkPoFxPowerWaits.emplace(
        Call.Handle, FrameworkPoFxPowerWait{Call.Token, Call.Kind});
    if (auto E = Framework->powerPolicyPermission(Call.PDO, !Required)) {
      FrameworkPoFxPowerWaits.erase(Call.Handle);
      return E;
    }
    // Returning the internal callback does not release its acknowledgement.
    // Device power completes it, or managed USB explicitly accepts or declines
    // NotRequired after retaining or cancelling its idle packet.
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
    const auto *Call = PoFx.callback(It->second.CallbackToken);
    if (!Registration || !Framework || !Call || !Call->Internal ||
        Call->Handle != It->first || Call->Kind != It->second.Kind)
      return policyError("pending power acknowledgement lost its owner");
    const bool Required =
        It->second.Kind == KernelPoFx::CallbackKind::DevicePowerRequired;
    auto Completion =
        Framework->powerPolicyDeviceCompletion(Registration->PDO, Required);
    if (!Completion)
      return Completion.takeError();
    if (!*Completion) {
      ++It;
      continue;
    }
    auto E = Required ? PoFx.reportDevicePoweredOn(It->first)
                      : PoFx.completeDevicePowerNotRequired(It->first);
    if (E)
      return E;
    It = FrameworkPoFxPowerWaits.erase(It);
    if (**Completion & profile::NTStatusFailureMask)
      return policyError("device power transaction failed with NTSTATUS 0x" +
                         llvm::utohexstr(**Completion));
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
