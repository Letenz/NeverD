//===- KernelModelAPC.cpp - Guest thread APC state ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Critical and guarded regions share the current guest thread's APC state.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
llvm::Error apcError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
} // namespace

llvm::Expected<KernelModel::ApcState *> KernelModel::apcStateForCall() {
  if (!CurrentExecution || !CurrentThreadKey)
    return apcError("APC state requires an active guest thread");
  return &ApcStates[CurrentThreadKey];
}

llvm::Expected<uint64_t> KernelModel::apcsDisabled() {
  auto State = apcStateForCall();
  if (!State)
    return State.takeError();
  bool OwnsMutex = false;
  for (const auto &[Execution, ThreadKey] : ExecutionThreadKeys)
    if (ThreadKey == CurrentThreadKey && Dispatcher.ownsMutex(Execution)) {
      OwnsMutex = true;
      break;
    }
  const bool PassiveInterrupt = std::any_of(
      PassiveInterruptThreads.begin(), PassiveInterruptThreads.end(),
      [&](const auto &Entry) { return Entry.second == CurrentThreadKey; });
  return uint64_t((*State)->CriticalDepth || (*State)->GuardedDepth ||
                  OwnsMutex || PassiveInterrupt);
}

llvm::Expected<uint64_t> KernelModel::allApcsDisabled() {
  auto State = apcStateForCall();
  if (!State)
    return State.takeError();
  return uint64_t((*State)->GuardedDepth || CurrentIRQL >= windows::APCLevel);
}

llvm::Expected<uint64_t> KernelModel::enterApcRegion(ApcRegionKind Kind) {
  auto State = apcStateForCall();
  if (!State)
    return State.takeError();
  const bool Critical = Kind == ApcRegionKind::Critical;
  auto &Depth = Critical ? (*State)->CriticalDepth : (*State)->GuardedDepth;
  if (Depth == profile::MaxAPCRegionNesting)
    return apcError(Critical ? "critical-region nesting limit exceeded"
                             : "guarded-region nesting limit exceeded");
  ++Depth;
  return 0;
}

llvm::Expected<uint64_t> KernelModel::leaveApcRegion(ApcRegionKind Kind) {
  auto State = apcStateForCall();
  if (!State)
    return State.takeError();
  const bool Critical = Kind == ApcRegionKind::Critical;
  auto &Depth = Critical ? (*State)->CriticalDepth : (*State)->GuardedDepth;
  if (!Depth)
    return apcError(Critical ? "KeLeaveCriticalRegion has no matching entry"
                             : "KeLeaveGuardedRegion has no matching entry");
  --Depth;
  return 0;
}
} // namespace neverd::emulation
