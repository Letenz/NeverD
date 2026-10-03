//===- KernelModelIRQL.cpp - Guest IRQL raise and restore -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Pair actual x64 WDK IRQL imports on one cooperative guest execution.
///
//===----------------------------------------------------------------------===//

#include "KernelAPINames.h"
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>
#include <iterator>

namespace neverd::emulation {
namespace {
llvm::Error irqlError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
} // namespace

llvm::Expected<uint64_t>
KernelModel::callIRQLAPI(llvm::StringRef Name,
                         llvm::ArrayRef<uint64_t> Arguments) {
  const uint8_t RequestedIRQL = static_cast<uint8_t>(Arguments[0]);
  if (!CurrentExecution)
    return irqlError("IRQL change requires an active guest execution");
  if (Name == kernel_api::KfRaiseIrql) {
    if (RequestedIRQL > dispatcher::HighLevel || RequestedIRQL < CurrentIRQL)
      return irqlError("KfRaiseIrql must raise to a valid IRQL");
    if (RaisedIRQLs.size() >= profile::MaxNestedIRQLRaises)
      return irqlError("nested IRQL raise limit exceeded");
    const uint8_t PreviousIRQL = CurrentIRQL;
    RaisedIRQLs.push_back(
        RaisedIRQL{CurrentExecution, PreviousIRQL, RequestedIRQL});
    CurrentIRQL = RequestedIRQL;
    return PreviousIRQL;
  }
  if (Name != kernel_api::KeLowerIrql)
    return irqlError("unknown IRQL operation");
  const auto Raise = std::find_if(
      RaisedIRQLs.rbegin(), RaisedIRQLs.rend(),
      [&](const auto &Entry) { return Entry.Execution == CurrentExecution; });
  if (Raise == RaisedIRQLs.rend() || Raise->NewIRQL != CurrentIRQL ||
      Raise->OldIRQL != RequestedIRQL)
    return irqlError("KeLowerIrql requires the latest saved IRQL on this "
                     "execution");
  if (RequestedIRQL < scheduler::DispatchLevel) {
    if (CancelLock.Held)
      return irqlError("cannot lower IRQL while holding the cancel spin lock");
    for (const auto &[Address, Lock] : ExecutiveSpinLocks)
      if (Lock.Execution == CurrentExecution)
        return irqlError(
            "cannot lower IRQL while holding an executive spin lock");
  }
  if (auto Required = Interrupts.manualHoldIRQL(CurrentExecution);
      Required && RequestedIRQL < *Required)
    return irqlError("cannot lower IRQL while holding an interrupt spin lock");
  RaisedIRQLs.erase(std::next(Raise).base());
  CurrentIRQL = RequestedIRQL;
  return 0;
}

llvm::Expected<uint64_t> KernelModel::callApcStateAPI(llvm::StringRef Name) {
  if (!CurrentExecution || !CurrentThreadKey)
    return irqlError("APC state requires an active guest thread");
  auto &State = ApcStates[CurrentThreadKey];
  if (Name == kernel_api::KeAreApcsDisabled) {
    bool OwnsMutex = false;
    for (const auto &[Execution, ThreadKey] : ExecutionThreadKeys)
      if (ThreadKey == CurrentThreadKey && Dispatcher.ownsMutex(Execution)) {
        OwnsMutex = true;
        break;
      }
    const bool PassiveInterrupt = std::any_of(
        PassiveInterruptThreads.begin(), PassiveInterruptThreads.end(),
        [&](const auto &Entry) { return Entry.second == CurrentThreadKey; });
    return uint64_t(State.CriticalDepth || State.GuardedDepth || OwnsMutex ||
                    PassiveInterrupt);
  }
  if (Name == kernel_api::KeAreAllApcsDisabled)
    return uint64_t(State.GuardedDepth || CurrentIRQL >= windows::APCLevel);
  if (Name == kernel_api::KeEnterCriticalRegion) {
    if (State.CriticalDepth == profile::MaxAPCRegionNesting)
      return irqlError("critical-region nesting limit exceeded");
    ++State.CriticalDepth;
    return 0;
  }
  if (Name == kernel_api::KeLeaveCriticalRegion) {
    if (!State.CriticalDepth)
      return irqlError("KeLeaveCriticalRegion has no matching entry");
    --State.CriticalDepth;
    return 0;
  }
  if (Name == kernel_api::KeEnterGuardedRegion) {
    if (State.GuardedDepth == profile::MaxAPCRegionNesting)
      return irqlError("guarded-region nesting limit exceeded");
    ++State.GuardedDepth;
    return 0;
  }
  if (Name == kernel_api::KeLeaveGuardedRegion) {
    if (!State.GuardedDepth)
      return irqlError("KeLeaveGuardedRegion has no matching entry");
    --State.GuardedDepth;
    return 0;
  }
  return irqlError("unknown APC state operation");
}
} // namespace neverd::emulation
