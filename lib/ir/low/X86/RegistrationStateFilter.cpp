//===- RegistrationStateFilter.cpp - Resumed SEH filter state -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/lift/X86Regs.h"

#include <utility>

namespace neverd::registration_state {

bool RegistrationStateSolver::applySEHContinueFilters(size_t I, Domain &State,
                                                      const BlockFacts &Facts) {
  const auto SP = State.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride]
                      .CallbackAddress;
  if (!SP || State.Frame.loadCallback(SP->Offset + 4, 4).Constant != 0 ||
      State.Levels.empty())
    return false;

  SEHFrameOutcomes Continued;
  for (int32_t Level : State.Levels) {
    if (!validLevel(Level) ||
        !charge(State.Frame.cellCount() +
                State.Frame.OtherRegisterBytes.size() + 10))
      return false;
    FrameState Frame = State.Frame;
    Frame.store(*Chain.TryLevelOffset, 4,
                FrameValue::constant(uint32_t(Level)));
    if (!runSEHFilterSearch(Frame, nullptr, Continued,
                            /*RequireResume=*/false))
      return false;
  }

  // If every filter selects a handler, ordinary fallthrough is unreachable.
  // Keeping its existing overapproximation cannot supply a returning frame;
  // nonlocal-unwind proofs independently require a closed resumption.
  if (Continued.empty())
    return true;

  // Publish the checked returning effects only after every local search.
  // A callback can activate a scope which no ordinary parent block entered.
  // Its exceptional roots need the actual returned frame, before joining
  // different filter-written levels for subsequent ordinary instructions.
  std::optional<FrameState> Joined;
  State.Levels.clear();
  for (const auto &[Level, Frame] : Continued) {
    if (!charge(Frame.cellCount() + Frame.OtherRegisterBytes.size() +
                State.catchCellCount() + State.sehCallerCellCount() + 10))
      return false;
    Domain Resumed = State;
    Resumed.Levels = {Level};
    Resumed.Frame = Frame;
    dispatchBlock(I, Resumed, Facts);
    State.Levels.insert(Level);
    if (!Joined)
      Joined = Frame;
    else
      Joined->merge(Frame);
  }
  if (!Joined)
    return false;
  State.Frame = std::move(*Joined);
  return !Exhausted;
}

} // namespace neverd::registration_state
