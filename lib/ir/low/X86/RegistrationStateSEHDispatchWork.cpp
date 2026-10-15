//===- RegistrationStateSEHDispatchWork.cpp - SEH dispatch worklist -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Batches equal source snapshots at ordinary fixed-point boundaries.

#include "RegistrationStateSolver.h"

#include "llvm/ADT/Hashing.h"

#include <bit>

namespace neverd::registration_state {

void RegistrationStateSolver::queueSEHScopes(const Domain &Before,
                                             bool Invalid) {
  const auto &Reaching = Before.Unknown || Invalid ? AllLevels : Before.Levels;
  if (Reaching.empty() || *Reaching.rbegin() < 0)
    return;
  if (!charge(Before.snapshotWork() + Reaching.size() + 1))
    return;
  Domain Snapshot = dispatchSource(Before);
  // dispatch replaces this exact cell with the selected root level. Every
  // inherited fact stays in the key, including captures, parent cells and
  // unknown provenance. The same owner projects facts for dispatch itself,
  // so caller registers that the runtime replaces cannot split this key.
  Snapshot.Frame.Cells.erase(*Chain.TryLevelOffset);
  if (!charge(Snapshot.snapshotWork() + 1 +
              2 * std::bit_width(SEHFrameBuckets.size() + 1)))
    return;
  const size_t Hash = llvm::hash_combine(Snapshot.snapshotHash(), Invalid);
  auto &Candidates = SEHFrameBuckets[Hash];
  size_t Index = DispatchedSEHFrames.size();
  for (size_t CandidateIndex : Candidates) {
    const auto &Candidate = DispatchedSEHFrames[CandidateIndex];
    if (!charge(Snapshot.snapshotWork() + Candidate.Source.snapshotWork()))
      return;
    // Hash collisions never merge states. Full equality also keeps a newly
    // added domain field authoritative before its hash is updated.
    if (Candidate.Invalid == Invalid && Candidate.Source == Snapshot) {
      Index = CandidateIndex;
      break;
    }
  }
  if (Index == DispatchedSEHFrames.size()) {
    DispatchedSEHFrames.push_back({std::move(Snapshot), Invalid, {}, {}});
    Candidates.push_back(Index);
  }
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
