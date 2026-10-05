//===- KernelModelPoFxCalls.cpp - PoFx component operations --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
llvm::Error poFxError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "PoFx: " + Message);
}
} // namespace

llvm::Error KernelModel::preparePoFxOperation(uint64_t Handle,
                                              PoFxHandleAccess Access) {
  if (auto E = PoFx.process(Scheduler.now100ns()))
    return E;
  const auto *Registration = PoFx.registration(Handle);
  if (!Registration)
    return poFxError("operation requires a live PoFx handle");
  if (Registration->Owner == KernelPoFx::RegistrationOwner::Framework &&
      Access == PoFxHandleAccess::DriverOwned)
    return poFxError(
        "operation is owned by the framework registration in this profile");
  return llvm::Error::success();
}

llvm::Expected<uint64_t> KernelModel::finishPoFxOperation(llvm::Error Error) {
  if (Error)
    return std::move(Error);
  if (auto E = queuePoFxCallbacks())
    return E;
  return 0;
}

llvm::Expected<uint64_t>
KernelModel::changePoFxComponent(uint64_t Handle, uint32_t Index,
                                 uint32_t Flags, PoFxCondition Condition) {
  if (auto E = preparePoFxOperation(Handle, PoFxHandleAccess::DriverOwned))
    return E;
  if (Flags & ~(pofx::FlagBlocking | pofx::FlagAsyncOnly) ||
      Flags == (pofx::FlagBlocking | pofx::FlagAsyncOnly))
    return poFxError("component operation has invalid flags");
  const bool Blocking = Flags & pofx::FlagBlocking;
  if (Blocking &&
      (CurrentIRQL >= scheduler::DispatchLevel || !CurrentThreadKey ||
       BlockingPoFx.contains(CurrentThreadKey)))
    return poFxError("blocking component operation requires an available "
                     "caller thread below DISPATCH_LEVEL");
  const bool Active = Condition == PoFxCondition::Active;
  llvm::Error E =
      Active ? PoFx.activate(Handle, Index, Blocking ? CurrentThreadKey : 0)
             : PoFx.idle(Handle, Index, Blocking ? CurrentThreadKey : 0);
  if (!E && Blocking) {
    auto Component = PoFx.component(Handle, Index);
    if (!Component)
      return Component.takeError();
    BlockingPoFxOperation Operation{
        Handle, CurrentThreadKey, Index, Active, {}};
    // Releasing a nested activation reference does not request a condition
    // change and must not wait for other users to release their references.
    Operation.Completed = !Active && Component->References != 0;
    Operation.CompletionGeneration =
        Active ? Component->ActiveGeneration : Component->IdleGeneration;
    BlockingPoFx.emplace(CurrentThreadKey, std::move(Operation));
  }
  if (E)
    return std::move(E);
  if (auto Error = queuePoFxCallbacks())
    return Error;
  if (Blocking)
    if (auto E = waitForPoFxOperation(CurrentThreadKey))
      return E;
  return 0;
}

llvm::Expected<uint64_t>
KernelModel::startPoFxPowerManagement(uint64_t Handle) {
  if (auto E = preparePoFxOperation(Handle, PoFxHandleAccess::DriverOwned))
    return E;
  return finishPoFxOperation(PoFx.start(Handle));
}

llvm::Expected<uint64_t>
KernelModel::completePoFxIdleCondition(uint64_t Handle, uint32_t Index) {
  if (auto E = preparePoFxOperation(Handle, PoFxHandleAccess::AnyOwner))
    return E;
  return finishPoFxOperation(PoFx.completeIdleCondition(Handle, Index));
}

llvm::Expected<uint64_t> KernelModel::completePoFxIdleState(uint64_t Handle,
                                                            uint32_t Index) {
  if (auto E = preparePoFxOperation(Handle, PoFxHandleAccess::AnyOwner))
    return E;
  return finishPoFxOperation(PoFx.completeIdleState(Handle, Index));
}

llvm::Expected<uint64_t>
KernelModel::completePoFxPowerNotRequired(uint64_t Handle) {
  if (auto E = preparePoFxOperation(Handle, PoFxHandleAccess::DriverOwned))
    return E;
  return finishPoFxOperation(PoFx.completeDevicePowerNotRequired(Handle));
}

llvm::Expected<uint64_t>
KernelModel::reportPoFxDevicePoweredOn(uint64_t Handle) {
  if (auto E = preparePoFxOperation(Handle, PoFxHandleAccess::DriverOwned))
    return E;
  return finishPoFxOperation(PoFx.reportDevicePoweredOn(Handle));
}

llvm::Expected<uint64_t>
KernelModel::setPoFxComponentLatency(uint64_t Handle, uint32_t Index,
                                     uint64_t Latency) {
  if (auto E = preparePoFxOperation(Handle, PoFxHandleAccess::AnyOwner))
    return E;
  return finishPoFxOperation(PoFx.setLatency(Handle, Index, Latency));
}

llvm::Expected<uint64_t>
KernelModel::setPoFxComponentResidency(uint64_t Handle, uint32_t Index,
                                       uint64_t Residency) {
  if (auto E = preparePoFxOperation(Handle, PoFxHandleAccess::AnyOwner))
    return E;
  return finishPoFxOperation(PoFx.setResidency(Handle, Index, Residency));
}

llvm::Expected<uint64_t>
KernelModel::setPoFxComponentWake(uint64_t Handle, uint32_t Index, bool Wake) {
  if (auto E = preparePoFxOperation(Handle, PoFxHandleAccess::AnyOwner))
    return E;
  return finishPoFxOperation(PoFx.setWake(Handle, Index, Wake));
}

llvm::Expected<uint64_t>
KernelModel::setPoFxDeviceIdleTimeout(uint64_t Handle, uint64_t Timeout) {
  if (auto E = preparePoFxOperation(Handle, PoFxHandleAccess::DriverOwned))
    return E;
  return finishPoFxOperation(PoFx.setDeviceIdleTimeout(Handle, Timeout));
}

} // namespace neverd::emulation
