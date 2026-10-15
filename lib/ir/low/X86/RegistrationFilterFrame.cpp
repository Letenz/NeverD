//===- RegistrationFilterFrame.cpp - Continuing SEH search effects -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Separates returning filter effects from paths which select a handler.

#include "RegistrationStateSolver.h"

#include <deque>

namespace neverd::registration_state {

bool RegistrationStateSolver::runSEHFilterSearch(
    const FrameState &Parent, std::vector<RegistrationObjectExtent> *Writes,
    SEHFrameOutcomes &Outcomes, bool RequireResume) {
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
                Current.Frame.OtherRegisterBytes.size() + 10))
      return false;
    if (Current.Cursor < 0) {
      // A search outside this registration can return through an external
      // handler. It contributes ordinary effects but cannot close a bounded
      // nonlocal-unwind proof.
      if (RequireResume ||
          !mergeSEHFrameOutcome(Outcomes, std::move(Current.Frame)))
        return false;
      continue;
    }
    if (size_t(Current.Cursor) >= Chain.Scopes.size() ||
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
        } else if (RequireResume) {
          // A selected handler abandons this invocation. It contributes no
          // ordinary returning frame, and prevents a closed resumption proof.
          return false;
        }
      }
  }
  return !RequireResume || !Outcomes.empty();
}

} // namespace neverd::registration_state
