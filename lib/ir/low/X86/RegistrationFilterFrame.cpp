//===- RegistrationFilterFrame.cpp - Continuing SEH search effects -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Propagates checked filter effects back to a suspended finally invocation.

#include "RegistrationStateSolver.h"

namespace neverd::registration_state {

bool RegistrationStateSolver::runSEHContinueFilters(
    FrameState &Parent, std::vector<RegistrationObjectExtent> *Writes) {
  if (KnownCxx || EH4 || !Chain.TryLevelOffset)
    return false;
  const auto Level = Parent.load(*Chain.TryLevelOffset, 4).Constant;
  if (!Level)
    return false;
  int32_t Walk = int32_t(*Level);
  size_t Steps = 0;
  while (Walk != -1) {
    if (!charge(Parent.cellCount() + 1) || Walk < 0 ||
        size_t(Walk) >= Chain.Scopes.size() || ++Steps > Chain.Scopes.size())
      return false;
    const auto &Scope = Chain.Scopes[Walk];
    Walk = Scope.EnclosingLevel;
    if (Scope.IsFinally)
      continue;
    FrameValue Result;
    if (!Scope.FilterVA ||
        !runSEHCallbackFrame(Scope.FilterVA, Parent, false, Writes, nullptr,
                             nullptr, &Result) ||
        Result.MayBeFrame || !Result.Constant)
      return false;
    if (*Result.Constant == UINT32_MAX)
      return true;
    // A selected handler, nonlocal transfer or unknown disposition requires
    // its own transition. Search cannot create an ordinary returning fact.
    if (*Result.Constant != 0)
      return false;
  }
  return false;
}

} // namespace neverd::registration_state
