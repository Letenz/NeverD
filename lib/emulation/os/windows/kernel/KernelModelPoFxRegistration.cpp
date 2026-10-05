//===- KernelModelPoFxRegistration.cpp - PoFx device registration --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
llvm::Error poFxError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "PoFx: " + Message);
}
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

llvm::Expected<uint64_t> KernelModel::registerPoFxDevice(uint64_t Object,
                                                         uint64_t Record,
                                                         uint64_t Output) {
  if (auto E = PoFx.process(Scheduler.now100ns()))
    return E;
  const auto Device = Devices.find(Object);
  if (Device == Devices.end() || Device->second.DeletePending)
    return poFxError("registration requires a live device object");
  auto PDO = pnpDeviceForRoute(Object);
  if (!PDO)
    return PDO.takeError();
  if (!*PDO)
    return poFxError("registration requires a configured PnP provider");
  if (*PDO != Object)
    return poFxError("registration requires the exact configured PDO");
  if (auto E = validatePoFxRegistrationDevice(*PDO))
    return E;
  if (auto E = validateGuestAccess(Output, profile::PointerSize, true))
    return E;
  auto Writable =
      Memory.canAccess(Output, profile::PointerSize, GuestPermission::Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return poFxError("registration output is not writable");
  auto Description = readPoFxRegistration(*PDO, Record);
  if (!Description)
    return Description.takeError();
  const uint64_t Candidate =
      (NextAllocation + profile::PointerSize - 1) & ~(profile::PointerSize - 1);
  if (auto E = PoFx.canRegisterDevice(Candidate, *Description))
    return E;
  auto Handle = allocate(profile::PointerSize, profile::PointerSize);
  if (!Handle)
    return Handle.takeError();
  if (auto E = PoFx.registerDevice(*Handle, std::move(*Description)))
    return E;
  PoFxDeviceObjects.emplace(*Handle, Object);
  if (auto E = Memory.writeInteger(Output, *Handle, profile::PointerSize))
    return E;
  return windows::StatusSuccess;
}

llvm::Expected<uint64_t> KernelModel::unregisterPoFxDevice(uint64_t Handle) {
  if (auto E = preparePoFxOperation(Handle, PoFxHandleAccess::DriverOwned))
    return E;
  if (std::any_of(
          BlockingPoFx.begin(), BlockingPoFx.end(),
          [&](const auto &Entry) { return Entry.second.Handle == Handle; }))
    return poFxError("registration is retained by a blocking caller");
  auto E = PoFx.unregisterDevice(Handle);
  if (!E) {
    PoFxDeviceObjects.erase(Handle);
    FreedRanges.emplace(Handle, profile::PointerSize);
  }
  return finishPoFxOperation(std::move(E));
}
} // namespace neverd::emulation
