//===- LLVMScalarLoopWidths.cpp - Internal integer-width proposals --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LLVMScalarLoopRecoveryInternal.h"

#include "llvm/IR/Instructions.h"

#include <algorithm>

namespace neverd::analysis::scalar_recovery {
using namespace llvm;

bool widths(Search &S, Function &F, LoopInfo &LI) {
  if (LI.empty() || !S.charge(1 + F.arg_size()))
    return false;
  unsigned Width = F.getReturnType()->getIntegerBitWidth();
  for (auto &A : F.args())
    Width = std::max(Width, A.getType()->getIntegerBitWidth());
  bool Wider = false;
  for (auto &B : F)
    for (auto &I : B) {
      if (!S.charge(1 + I.getNumOperands()))
        return false;
      Wider |= I.getType()->isIntegerTy() &&
               I.getType()->getIntegerBitWidth() > Width;
      if (auto *Call = dyn_cast<CallInst>(&I)) {
        // Keep every declared intrinsic signature intact. Aggregate and
        // noninteger results need a separate type-aware candidate policy.
        if (!Call->getType()->isIntegerTy() ||
            Call->getType()->getIntegerBitWidth() > Width)
          return false;
        for (auto &A : Call->args())
          if (!A->getType()->isIntegerTy() ||
              A->getType()->getIntegerBitWidth() > Width)
            return false;
      }
    }
  if (!Wider)
    return false;
  Candidate C;
  if (!S.clone(F, C, false, Width))
    return false;
  // The external interface only nominates a width. High bits, signed
  // order, shifts, poison and all iterations remain proof obligations.
  return S.accept(C);
}
} // namespace neverd::analysis::scalar_recovery
