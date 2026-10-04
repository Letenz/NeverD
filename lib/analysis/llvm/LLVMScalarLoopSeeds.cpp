//===- LLVMScalarLoopSeeds.cpp - Entry-value carrier proposals ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LLVMScalarLoopRecoveryInternal.h"

namespace neverd::analysis::scalar_recovery {
using namespace llvm;
bool seeds(Search &S, Function &F, DominatorTree &DT, LoopInfo &LI) {
  SmallVector<Replacement, MaxReplacementBatch> Slots;
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
      if (S.replacementDeferred(P))
        continue;
      Candidate C;
      if (!S.clone(F, C))
        return false;
      substitute(C, {{&P, Seed}});
      if (S.screen(C))
        Slots.push_back({&P, Seed});
      else if (!S.deferReplacement(P))
        return false;
      if (S.stopped())
        return false;
      if (Slots.size() == MaxReplacementBatch) {
        if (proposeReplacements(S, F, Slots))
          return true;
        if (S.stopped())
          return false;
        Slots.clear();
      }
    }
  }
  // Agreement at every external entry only nominates the value. All actual
  // backedges, definedness and termination remain part of the full proof.
  return proposeReplacements(S, F, Slots);
}
} // namespace neverd::analysis::scalar_recovery
