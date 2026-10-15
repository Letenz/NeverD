//===- RegistrationFilterFrame.cpp - Continuing SEH search effects -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Propagates checked filter effects back to a suspended finally invocation.

#include "RegistrationStateSolver.h"

#include <deque>

namespace neverd::registration_state {

bool RegistrationStateSolver::runSEHContinueFilters(
    const FrameState &Parent, std::vector<RegistrationObjectExtent> *Writes,
    SEHFrameOutcomes &Outcomes) {
  if (KnownCxx || EH4 || !Chain.TryLevelOffset)
    return false;
  const auto Level = Parent.load(*Chain.TryLevelOffset, 4).Constant;
  if (!Level)
    return false;
  struct Search {
    FrameState Frame;
    int32_t Cursor;
    size_t Steps;
  };
  std::deque<Search> Pending{{Parent, int32_t(*Level), 0}};
  while (!Pending.empty()) {
    Search Current = std::move(Pending.front());
    Pending.pop_front();
    if (!charge(Current.Frame.cellCount() +
                Current.Frame.OtherRegisterBytes.size() + 10) ||
        Current.Cursor < 0 || size_t(Current.Cursor) >= Chain.Scopes.size() ||
        ++Current.Steps > Chain.Scopes.size())
      return false;
    const auto &Scope = Chain.Scopes[Current.Cursor];
    if (Scope.IsFinally) {
      Pending.push_back(
          {std::move(Current.Frame), Scope.EnclosingLevel, Current.Steps});
      continue;
    }
    SEHFilterOutcomes Returned;
    if (!Scope.FilterVA ||
        !runSEHCallbackFrame(Scope.FilterVA, Current.Frame, false, Writes,
                             nullptr, nullptr, &Returned))
      return false;
    for (auto &[Disposition, Frames] : Returned)
      for (auto &[State, Frame] : Frames) {
        // EH3 tests EAX's sign bit; any negative LONG continues execution.
        if (int32_t(Disposition) < 0) {
          if (!mergeSEHFrameOutcome(Outcomes, std::move(Frame)))
            return false;
        } else if (Disposition == 0) {
          // The runtime captured the search cursor before invoking the
          // filter. Its state write affects later unwind, not this cursor.
          Pending.push_back(
              {std::move(Frame), Scope.EnclosingLevel, Current.Steps});
        } else {
          // A selected handler, nonlocal transfer or unknown disposition
          // cannot create an ordinary returning fact. Every path must resume
          // this invocation; escaping the parent's search also refuses.
          return false;
        }
      }
  }
  return !Outcomes.empty();
}

} // namespace neverd::registration_state
