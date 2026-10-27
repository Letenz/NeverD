//===- RegistrationStateLocalUnwindReceipt.cpp - Local unwind calls ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve the actual arguments and state outcomes of each CRT invocation.

#include "RegistrationStateSolver.h"

#include <algorithm>
#include <iterator>

namespace neverd::registration_state {

void RegistrationStateSolver::recordLocalUnwind(
    const Domain &State, RegistrationSEHLocalUnwind Call) {
  if (!Result.SEHLocalUnwindsComplete)
    return;
  if (State.Parent && !State.Callback) {
    Call.EntryVA = 0;
  } else if (!State.Parent && State.Callback &&
             !State.Activation.SearchFilter && State.Activation.Entry &&
             State.Activation.Entry == State.Frame.CallbackEntry &&
             FinallyTargets.count(*State.Activation.Entry)) {
    // OtherCallback distinguishes SEH entries from C++ catch invocations.
    // Both an ordinary CALL and CRT dispatch own this same finally body.
    Call.EntryVA = *State.Activation.Entry;
  } else {
    Result.SEHLocalUnwindsComplete = false;
    return;
  }
  if (Call.Transitions.empty() || !charge(Call.Transitions.size() + 1)) {
    Result.SEHLocalUnwindsComplete = false;
    return;
  }
  const auto [Found, New] =
      LocalUnwindCalls.emplace(std::make_pair(Call.Address, Call.OpSeq), Call);
  if (New)
    return;
  if (!charge(Found->second.Transitions.size() + Call.Transitions.size() + 1)) {
    Result.SEHLocalUnwindsComplete = false;
    return;
  }
  auto Previous = Found->second;
  Previous.Transitions = Call.Transitions;
  if (Previous != Call) {
    Result.SEHLocalUnwindsComplete = false;
    return;
  }
  auto &Transitions = Found->second.Transitions;
  std::vector<std::pair<int32_t, int32_t>> Combined;
  std::set_union(Transitions.begin(), Transitions.end(),
                 Call.Transitions.begin(), Call.Transitions.end(),
                 std::back_inserter(Combined));
  Transitions = std::move(Combined);
}

} // namespace neverd::registration_state
