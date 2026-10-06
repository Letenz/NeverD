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

llvm::Expected<uint64_t> KernelModel::raiseIRQL(uint8_t RequestedIRQL) {
  if (!CurrentExecution)
    return irqlError("IRQL change requires an active guest execution");
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

llvm::Expected<uint64_t> KernelModel::lowerIRQL(uint8_t RequestedIRQL) {
  if (!CurrentExecution)
    return irqlError("IRQL change requires an active guest execution");
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
} // namespace neverd::emulation
