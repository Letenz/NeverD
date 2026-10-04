//===- LLVMScalarLoopMasks.cpp - Carrier-mask proposals ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LLVMScalarLoopRecoveryInternal.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"

namespace neverd::analysis::scalar_recovery {
using namespace llvm;

bool masks(Search &S, Function &F, LoopInfo &LI) {
  SmallVector<Replacement, MaxReplacementBatch> Slots;
  for (auto &B : F)
    for (auto &I : B) {
      if (!S.charge(1 + I.getNumOperands()))
        return false;
      if (I.getOpcode() != Instruction::And || S.replacementDeferred(I))
        continue;
      Value *Input = I.getOperand(0);
      auto *Mask = dyn_cast<ConstantInt>(I.getOperand(1));
      if (!Mask) {
        Mask = dyn_cast<ConstantInt>(Input);
        Input = I.getOperand(1);
      }
      auto *Phi = dyn_cast<PHINode>(Input);
      auto *L = Phi ? LI.getLoopFor(Phi->getParent()) : nullptr;
      if (!Mask || Mask->isZero() || Mask->isMinusOne() || !L ||
          L->getHeader() != Phi->getParent())
        continue;
      Replacement R{&I, nullptr};
      // A header PHI can carry a constant field or a bounded recurrence.
      // Neither shape proves a range. Screen zero and identity proposals
      // over the full source control domain, keeping any alternative for
      // full-data validation when the preferred value fails.
      for (Value *Proposed :
           {static_cast<llvm::Value *>(ConstantInt::get(Mask->getType(), 0)),
            Input}) {
        Candidate C;
        if (!S.clone(F, C))
          return false;
        substitute(C, {{&I, Proposed}});
        if (S.screen(C)) {
          if (!R.Preferred)
            R.Preferred = Proposed;
          else
            R.Alternative = Proposed;
        }
        if (S.stopped())
          return false;
      }
      if (R.Preferred)
        Slots.push_back(R);
      else if (!S.deferReplacement(I))
        return false;
      if (Slots.size() == MaxReplacementBatch) {
        if (proposeReplacements(S, F, Slots))
          return true;
        if (S.stopped())
          return false;
        Slots.clear();
      }
    }
  return proposeReplacements(S, F, Slots);
}
} // namespace neverd::analysis::scalar_recovery
