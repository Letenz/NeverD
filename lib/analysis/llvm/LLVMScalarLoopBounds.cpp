//===- LLVMScalarLoopBounds.cpp - Scalar exit-bound proposals -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LLVMScalarLoopRecoveryInternal.h"

#include "llvm/IR/IRBuilder.h"

namespace neverd::analysis::scalar_recovery {
using namespace llvm;
namespace {
constexpr unsigned MaxValues = 32;
constexpr unsigned MaxOperands = 64;
constexpr unsigned MaxBounds = 4;

// This slice only discovers candidates. An invariant leaf supplies no range
// assumption, and syntactic dependence does not prove any counter relation.
SmallVector<Value *, MaxBounds> findBounds(Search &S, Loop &L,
                                           DominatorTree &DT,
                                           Instruction &Condition,
                                           PHINode &Counter) {
  SmallVector<Value *, MaxValues> Work{&Condition};
  SmallPtrSet<Value *, MaxValues> Seen;
  SmallVector<Value *, MaxBounds> Bounds;
  bool UsesCounter = false;
  for (size_t N = 0; N < Work.size(); ++N) {
    if (!S.charge())
      return {};
    auto *V = Work[N];
    if (!Seen.insert(V).second)
      continue;
    if (Seen.size() > MaxValues)
      return {};
    if (V == &Counter) {
      UsesCounter = true;
      continue;
    }
    if (isa<Constant>(V))
      continue;
    if (L.isLoopInvariant(V)) {
      if (V->getType() == Counter.getType() && DT.dominates(V, &Condition) &&
          Bounds.size() < MaxBounds)
        Bounds.push_back(V);
      continue;
    }
    auto *I = dyn_cast<Instruction>(V);
    if (!I || isa<PHINode>(I))
      continue;
    if (Work.size() + I->getNumOperands() > MaxOperands)
      return {};
    for (auto *O : I->operand_values())
      Work.push_back(O);
  }
  if (!UsesCounter)
    Bounds.clear();
  return Bounds;
}
} // namespace

bool bounds(Search &S, Function &F, DominatorTree &DT, LoopInfo &LI) {
  for (auto *L : LI.getLoopsInPreorder()) {
    if (!S.charge())
      return false;
    auto *Pre = L->getLoopPredecessor(), *Latch = L->getLoopLatch();
    if (!Pre || !Latch)
      continue;
    SmallVector<BasicBlock *, 4> Exiting;
    L->getExitingBlocks(Exiting);
    for (auto *X : Exiting) {
      if (!S.charge())
        return false;
      auto *Branch = dyn_cast<CondBrInst>(X->getTerminator());
      if (!Branch || L->contains(Branch->getSuccessor(0)) ==
                         L->contains(Branch->getSuccessor(1)))
        continue;
      auto *Condition = dyn_cast<Instruction>(Branch->getCondition());
      if (!Condition || isa<PHINode>(Condition))
        continue;
      for (auto &P : L->getHeader()->phis()) {
        if (!S.charge(1 + P.getNumIncomingValues()))
          return false;
        if (P.getNumIncomingValues() != 2 || !P.getType()->isIntegerTy() ||
            P.getType()->isIntegerTy(1))
          continue;
        auto *Update =
            dyn_cast<BinaryOperator>(P.getIncomingValueForBlock(Latch));
        if (!Update || Update->getOpcode() != Instruction::Add)
          continue;
        Value *A = Update->getOperand(0), *B = Update->getOperand(1);
        if (B == &P)
          std::swap(A, B);
        auto *Step = dyn_cast<ConstantInt>(B);
        if (A != &P || !Step || Step->isZero())
          continue;
        // Existing direct tests already supply a boundary to later phases.
        if (auto *Cmp = dyn_cast<ICmpInst>(Condition))
          if (Cmp->getOperand(0) == &P || Cmp->getOperand(1) == &P ||
              Cmp->getOperand(0) == Update || Cmp->getOperand(1) == Update)
            continue;
        auto Bounds = findBounds(S, *L, DT, *Condition, P);
        if (S.stopped())
          return false;
        for (auto *Bound : Bounds) {
          for (unsigned Offset = 0; Offset < 3; ++Offset) {
            Candidate C;
            if (!S.clone(F, C) || !S.charge(4))
              return false;
            auto *Old = C.get(Condition);
            IRBuilder<> Builder(Old);
            auto *Limit = C.map(Bound);
            if (Offset)
              Limit = Builder.CreateAdd(
                  Limit,
                  ConstantInt::get(F.getContext(), Offset == 1
                                                       ? -Step->getValue()
                                                       : Step->getValue()),
                  "loop.bound");
            auto *More =
                Builder.CreateICmpNE(C.get(&P), Limit, "loop.bound.test");
            if (!L->contains(Branch->getSuccessor(0)))
              More = Builder.CreateNot(More);
            Old->replaceAllUsesWith(More);
            Old->eraseFromParent();
            // The boundary or either one-step neighbor is only a proposal.
            // Full original-function proof still checks all source paths and
            // data, other uses of this condition, definedness and termination.
            if (S.accept(C))
              return true;
            if (S.stopped())
              return false;
          }
        }
      }
    }
  }
  return false;
}
} // namespace neverd::analysis::scalar_recovery
