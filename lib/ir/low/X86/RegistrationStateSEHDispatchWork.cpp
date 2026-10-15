//===- RegistrationStateSEHDispatchWork.cpp - SEH dispatch worklist -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Batches equal source snapshots at ordinary fixed-point boundaries.

#include "RegistrationStateSolver.h"

#include <bit>

namespace neverd::registration_state {

void RegistrationStateSolver::queueSEHScopes(const Domain &Before,
                                             bool Invalid) {
  const auto &Reaching = Before.Unknown || Invalid ? AllLevels : Before.Levels;
  if (Reaching.empty() || *Reaching.rbegin() < 0)
    return;
  if (!charge(Before.snapshotWork() + Reaching.size() + 1))
    return;
  Domain Snapshot = Before;
  Snapshot.Levels.clear();
  // dispatch replaces this exact cell with the selected root level. Every
  // other source fact stays in the key, including captures, invocation
  // identities, aliases and unknown provenance. No distinct frame is joined
  // here; it must contribute independently to every affected runtime root.
  Snapshot.Frame.Cells.erase(*Chain.TryLevelOffset);
  size_t Index = 0;
  for (; Index != DispatchedSEHFrames.size(); ++Index) {
    const auto &Candidate = DispatchedSEHFrames[Index];
    if (!charge(Snapshot.snapshotWork() + Candidate.Source.snapshotWork()))
      return;
    if (Candidate.Invalid == Invalid && Candidate.Source == Snapshot)
      break;
  }
  if (Index == DispatchedSEHFrames.size())
    DispatchedSEHFrames.push_back({std::move(Snapshot), Invalid, {}, {}});
  auto &Memo = DispatchedSEHFrames[Index];
  for (int32_t Level : Reaching) {
    if (!charge(1 + 2 * std::bit_width(Memo.Levels.size() + 1) +
                2 * std::bit_width(Memo.Pending.size() + 1)))
      return;
    if (Level >= 0 && !Memo.Levels.count(Level))
      Memo.Pending.insert(Level);
  }
  if (!Memo.Pending.empty() &&
      charge(1 + 2 * std::bit_width(PendingSEHFrames.size() + 1)))
    PendingSEHFrames.insert(Index);
}

void RegistrationStateSolver::flushSEHScopes() {
  // Ordinary propagation is quiescent. Collecting its complete contributions
  // before creating exceptional roots avoids repeatedly copying one frame
  // for every enclosing level. Those new roots resume the same fixed point.
  while (!PendingSEHFrames.empty() && !Exhausted) {
    if (!charge(1))
      return;
    const size_t Index = *PendingSEHFrames.begin();
    PendingSEHFrames.erase(PendingSEHFrames.begin());
    auto &Memo = DispatchedSEHFrames[Index];
    dispatchSEHScopes(Memo.Source, Memo.Invalid, Memo.Pending);
    // Only completed merges can suppress a later identical contribution.
    if (Exhausted || !charge(Memo.Pending.size() + 1))
      return;
    Memo.Levels.insert(Memo.Pending.begin(), Memo.Pending.end());
    Memo.Pending.clear();
  }
}

} // namespace neverd::registration_state
