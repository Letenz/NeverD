//===- RegistrationUnwindFrame.cpp - SEH unwind effects ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Replays the checked EH3 unwind protocol, reloading callback-written state.

#include "RegistrationStateSolver.h"

#include <deque>

namespace neverd::registration_state {

bool RegistrationStateSolver::runSEHUnwind(
    FrameState Frame, int32_t Level, uint32_t Bound, SEHFrameOutcomes &Outcomes,
    std::vector<RegistrationObjectExtent> &Writes, const EH3JumpCapture *Jump,
    const FrameValue *JumpValue, bool *StateInFrame) {
  if (KnownCxx || EH4 || !Chain.TryLevelOffset || !validLevel(Level))
    return false;
  struct Step {
    FrameState Frame;
    std::vector<FrameState> Ancestors;
    int32_t VisibleLevel;
  };
  Frame.store(*Chain.TryLevelOffset, 4, FrameValue::constant(uint32_t(Level)));
  std::deque<Step> Pending;
  Pending.push_back({std::move(Frame), {}, Level});
  while (!Pending.empty()) {
    Step Current = std::move(Pending.front());
    Pending.pop_front();
    const auto NativeLevel =
        Current.Frame.load(Chain.TryLevelOffset, 4).Constant;
    if (!NativeLevel || !validLevel(int32_t(*NativeLevel)) ||
        !charge(Current.Frame.cellCount() +
                Current.Frame.OtherRegisterBytes.size() + 10))
      return false;
    const int32_t Walk = int32_t(*NativeLevel);
    // Keep the original checked CRT's unsigned stop predicate. A callback can
    // retire or redirect the state, so its enclosing link is not the next
    // iteration's state unless the callback actually preserved that value.
    if (Walk == -1 || (Bound != UINT32_MAX && uint32_t(Walk) <= Bound)) {
      if (StateInFrame)
        *StateInFrame &= Walk == Current.VisibleLevel;
      if (!mergeSEHFrameOutcome(Outcomes, std::move(Current.Frame)))
        return false;
      continue;
    }
    if (Walk < 0 || size_t(Walk) >= Chain.Scopes.size())
      return false;
    for (const auto &Ancestor : Current.Ancestors) {
      if (!charge(Ancestor.cellCount() + Ancestor.OtherRegisterBytes.size() +
                  Current.Frame.cellCount() +
                  Current.Frame.OtherRegisterBytes.size() + 10) ||
          Ancestor == Current.Frame)
        return false;
    }
    // Re-entering a state with changed frame facts can be a finite protocol.
    // Exact recurrent configurations are refused; all other exploration,
    // including ancestor storage and branch fanout, shares the work budget.
    Current.Ancestors.push_back(Current.Frame);
    const auto &Scope = Chain.Scopes[Walk];
    Current.Frame.store(*Chain.TryLevelOffset, 4,
                        FrameValue::constant(uint32_t(Scope.EnclosingLevel)));
    SEHFrameOutcomes Returned;
    if (Scope.IsFinally) {
      if (!runLocalFinally(Scope.HandlerVA, Current.Frame, false, &Writes, Jump,
                           JumpValue, &Returned))
        return false;
    } else if (!mergeSEHFrameOutcome(Returned, std::move(Current.Frame))) {
      return false;
    }
    for (auto &[NextLevel, NextFrame] : Returned) {
      for (const auto &Ancestor : Current.Ancestors)
        if (!charge(Ancestor.cellCount() + Ancestor.OtherRegisterBytes.size() +
                    10))
          return false;
      // A completed finally exposes its current state to the source frame.
      // Skipping a filter changes only the runtime cursor; native resumption
      // must not reload the old logical cell after that step.
      Pending.push_back({std::move(NextFrame), Current.Ancestors,
                         Scope.IsFinally ? NextLevel : Current.VisibleLevel});
    }
  }
  return !Outcomes.empty();
}

} // namespace neverd::registration_state
