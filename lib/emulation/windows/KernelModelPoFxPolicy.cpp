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
} // namespace

llvm::Error KernelModel::setFrameworkPoFxIdle(uint64_t Device, bool Idle,
                                              uint64_t Timeout) {
  if (auto E = PoFx.process(Scheduler.now100ns()))
    return E;
  auto PDO = pnpDeviceForRoute(Device);
  if (!PDO)
    return PDO.takeError();
  auto Handle = PoFx.handleForPDO(*PDO);
  if (!Handle) {
    KernelPoFx::Registration Registration;
    Registration.PDO = *PDO;
    Registration.InternalCallbacks = true;
    KernelPoFx::Component Component;
    Component.IdleStates.push_back({0, 0, pofx::UnknownPower});
    Registration.Components.push_back(std::move(Component));
    const uint64_t Candidate = (NextAllocation + profile::PointerSize - 1) &
                               ~(profile::PointerSize - 1);
    if (auto E = PoFx.canRegisterDevice(Candidate, Registration))
      return E;
    auto Storage = allocate(profile::PointerSize, profile::PointerSize);
    if (!Storage)
      return Storage.takeError();
    if (auto E = PoFx.registerDevice(*Storage, std::move(Registration)))
      return E;
    PoFxDeviceObjects.emplace(*Storage, Device);
    Handle = *Storage;
    if (auto E = PoFx.activate(*Handle, 0))
      return E;
    if (auto E = PoFx.start(*Handle))
      return E;
  }
  if (!PoFx.registration(*Handle)->InternalCallbacks)
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
  if (!PoFx.registration(*Handle)->InternalCallbacks)
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
    if (auto E = completeFrameworkTransitionIfReady())
      return E;
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
