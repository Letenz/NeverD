//===- RegistrationUnwindFrame.cpp - SEH unwind effects ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Replays the checked SEH unwind protocol, reloading callback-written state.

#include "RegistrationStateSolver.h"

#include "neverd/loader/RegistrationScopeGraph.h"

#include <bit>
#include <cassert>
#include <map>
#include <vector>

namespace neverd::registration_state {

bool RegistrationStateSolver::runSEHUnwind(
    FrameState Frame, int32_t Level, uint32_t Bound, SEHFrameOutcomes &Outcomes,
    std::vector<RegistrationObjectExtent> &Writes, const SEHJumpCapture *Jump,
    const FrameValue *JumpValue,
    std::set<RegistrationSEHJumpState> *StateRestores) {
  if (KnownCxx || !Chain.TryLevelOffset || !validLevel(Level) ||
      (EH4 && (!CompleteCookies || !cookiesReady(Frame))))
    return false;
  struct Step {
    FrameState Frame;
    int32_t VisibleLevel;
    bool Leave = false;
  };
  Frame.store(*Chain.TryLevelOffset, 4, FrameValue::constant(uint32_t(Level)));
  std::vector<Step> Pending;
  Pending.push_back({std::move(Frame), Level});
  // Keep only the active DFS path. Copying the entire path into every next
  // step makes even a straight scope chain quadratic in frame storage/work.
  // Equal frames must have equal proved TryLevel cells, so other levels need
  // neither a frame comparison nor a separate copy of the ancestry.
  std::map<int32_t, std::vector<FrameState>> Ancestors;
  while (!Pending.empty()) {
    Step Current = std::move(Pending.back());
    Pending.pop_back();
    if (!charge(1 + 2 * std::bit_width(Ancestors.size() + 1)))
      return false;
    if (Current.Leave) {
      auto It = Ancestors.find(Current.VisibleLevel);
      assert(It != Ancestors.end() && !It->second.empty());
      const auto &Last = It->second.back();
      if (!charge(Last.cellCount() + Last.OtherRegisterBytes.size() + 1))
        return false;
      It->second.pop_back();
      if (It->second.empty())
        Ancestors.erase(It);
      continue;
    }
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
    const int32_t Outside = registrationOutsideTryLevel(EH4);
    if (Walk == Outside ||
        (Bound != uint32_t(Outside) && uint32_t(Walk) <= Bound)) {
      if (StateRestores) {
        if (!charge(1))
          return false;
        StateRestores->insert({Current.VisibleLevel, Walk});
      }
      if (!mergeSEHFrameOutcome(Outcomes, std::move(Current.Frame)))
        return false;
      continue;
    }
    if (Walk < 0 || size_t(Walk) >= Chain.Scopes.size())
      return false;
    auto &AtLevel = Ancestors[Walk];
    for (const auto &Ancestor : AtLevel) {
      if (!charge(Ancestor.cellCount() + Ancestor.OtherRegisterBytes.size() +
                  Current.Frame.cellCount() +
                  Current.Frame.OtherRegisterBytes.size() + 10) ||
          Ancestor == Current.Frame)
        return false;
    }
    // Re-entering a state with changed frame facts can be a finite protocol.
    // Exact recurrent configurations are refused; all other exploration,
    // including ancestor storage and branch fanout, shares the work budget.
    if (!charge(Current.Frame.cellCount() +
                Current.Frame.OtherRegisterBytes.size() + 10))
      return false;
    AtLevel.push_back(Current.Frame);
    // Leave after all children, so siblings may converge on the same frame
    // without borrowing each other's ancestry or hiding a recurrent path.
    Pending.push_back({{}, Walk, true});
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
    if (!charge(Returned.size() + 1))
      return false;
    for (auto It = Returned.rbegin(); It != Returned.rend(); ++It) {
      auto &[NextLevel, NextFrame] = *It;
      // A completed finally exposes its current state to the source frame.
      // Skipping a filter changes only the runtime cursor; native resumption
      // must project the old logical cell through those remaining steps.
      Pending.push_back({std::move(NextFrame),
                         Scope.IsFinally ? NextLevel : Current.VisibleLevel});
    }
  }
  return !Outcomes.empty();
}

} // namespace neverd::registration_state
