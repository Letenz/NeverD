//===- LLVMScalarLoopSeeds.cpp - Entry-value carrier proposals ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LLVMScalarLoopRecoveryInternal.h"

namespace neverd::analysis::scalar_recovery {
using namespace llvm;
namespace {
using Slot = std::pair<PHINode *, Value *>;
constexpr unsigned MaxBatch = 32;

void substitute(Candidate &C, ArrayRef<Slot> Slots) {
  for (auto [P, Seed] : Slots) {
    auto *Target = C.get(P);
    Target->replaceAllUsesWith(C.map(Seed));
    Target->eraseFromParent();
  }
}

bool propose(Search &S, Function &F, ArrayRef<Slot> Slots) {
  if (Slots.empty())
    return false;
  Candidate C;
  if (!S.clone(F, C))
    return false;
  substitute(C, Slots);
  // Batch acceptance still needs complete symbolic data. Individually
  // passing a zero-data screen proves neither a slot nor their composition.
  if (S.accept(C))
    return true;
  if (S.stopped())
    return false;
  if (Slots.size() == 1) {
    S.deferSeed(*Slots.front().first);
    return false;
  }
  const unsigned Mid = Slots.size() / 2;
  if (propose(S, F, Slots.take_front(Mid)))
    return true;
  if (S.stopped())
    return false;
  return propose(S, F, Slots.drop_front(Mid));
}
} // namespace

bool seeds(Search &S, Function &F, DominatorTree &DT, LoopInfo &LI) {
  SmallVector<Slot, MaxBatch> Slots;
  for (auto *L : LI.getLoopsInPreorder()) {
    if (!S.charge())
      return false;
    for (auto &P : L->getHeader()->phis()) {
      if (!S.charge(1 + P.getNumIncomingValues()))
        return false;
      Value *Seed = nullptr;
      bool Valid = true, HasBackedge = false;
      for (unsigned N = 0; N < P.getNumIncomingValues(); ++N) {
        if (L->contains(P.getIncomingBlock(N))) {
          HasBackedge = true;
          continue;
        }
        auto *Value = P.getIncomingValue(N);
        if (Seed && Seed != Value) {
          Valid = false;
          break;
        }
        Seed = Value;
      }
      if (!Valid || !Seed || !HasBackedge || !DT.dominates(Seed, &P))
        continue;
      if (S.seedDeferred(P))
        continue;
      Candidate C;
      if (!S.clone(F, C))
        return false;
      substitute(C, {{&P, Seed}});
      if (S.screen(C))
        Slots.push_back({&P, Seed});
      else if (!S.deferSeed(P))
        return false;
      if (S.stopped())
        return false;
      if (Slots.size() == MaxBatch) {
        if (propose(S, F, Slots))
          return true;
        if (S.stopped())
          return false;
        Slots.clear();
      }
    }
  }
  // Agreement at every external entry only nominates the value. All actual
  // backedges, definedness and termination remain part of the full proof.
  return propose(S, F, Slots);
}
} // namespace neverd::analysis::scalar_recovery
