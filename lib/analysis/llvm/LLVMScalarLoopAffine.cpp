//===- LLVMScalarLoopAffine.cpp - Affine carriers -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LLVMScalarLoopRecoveryInternal.h"

#include "llvm/IR/IRBuilder.h"

namespace neverd::analysis::scalar_recovery {
using namespace llvm;
namespace {
struct Recurrence {
  PHINode *Phi;
  APInt Seed, Step;
};

std::optional<Recurrence> recurrence(PHINode &P, Loop &L) {
  auto *Pre = L.getLoopPredecessor(), *Latch = L.getLoopLatch();
  if (!Pre || !Latch || P.getNumIncomingValues() != 2)
    return {};
  auto *Seed = dyn_cast<ConstantInt>(P.getIncomingValueForBlock(Pre));
  auto *Update = dyn_cast<BinaryOperator>(P.getIncomingValueForBlock(Latch));
  if (!Seed || !Update)
    return {};
  Value *A = Update->getOperand(0), *B = Update->getOperand(1);
  if (Update->getOpcode() == Instruction::Add && B == &P)
    std::swap(A, B);
  auto *Step = dyn_cast<ConstantInt>(B);
  if (A != &P || !Step)
    return {};
  if (Update->getOpcode() == Instruction::Add)
    return Recurrence{&P, Seed->getValue(), Step->getValue()};
  if (Update->getOpcode() == Instruction::Sub)
    return Recurrence{&P, Seed->getValue(), -Step->getValue()};
  return {};
}

std::optional<APInt> coefficient(const APInt &Base, const APInt &Target) {
  if (Base.isZero())
    return {};
  unsigned W = Base.getBitWidth(), Zeros = Base.countr_zero();
  if (!(Target & APInt::getLowBitsSet(W, Zeros)).isZero())
    return {};
  APInt Odd = Base.lshr(Zeros), Inverse(W, 1);
  for (unsigned N = 1; N < W; N *= 2)
    Inverse *= APInt(W, 2) - Odd * Inverse;
  APInt K = (Target.lshr(Zeros) * Inverse) & APInt::getLowBitsSet(W, W - Zeros);
  if (K.getSignificantBits() > 16 || K * Base != Target)
    return {};
  return K;
}
} // namespace

bool affine(Search &S, Function &F, LoopInfo &LI) {
  for (auto *L : LI.getLoopsInPreorder()) {
    SmallVector<Recurrence, 8> Recurrences, NonUnit;
    for (auto &P : L->getHeader()->phis()) {
      if (!S.charge())
        return false;
      if (auto R = recurrence(P, *L)) {
        auto &Group =
            R->Step.isOne() || R->Step.isAllOnes() ? Recurrences : NonUnit;
        Group.push_back(*R);
      }
    }
    if (!S.charge(NonUnit.size()))
      return false;
    llvm::append_range(Recurrences, NonUnit);
    // Prefer unit-step bases independently of PHI order, retaining source
    // order within each group. Whole-function proof, not the modular
    // coefficient alone, decides definedness of flagged source updates.
    for (unsigned J = 1; J < Recurrences.size(); ++J)
      for (unsigned I = 0; I < J; ++I) {
        if (!S.charge())
          return false;
        auto &X = Recurrences[I], &Y = Recurrences[J];
        if (X.Seed.getBitWidth() != Y.Seed.getBitWidth())
          continue;
        auto K = coefficient(X.Step, Y.Step);
        if (!K)
          continue;
        APInt Offset = Y.Seed - *K * X.Seed;
        Candidate C;
        if (!S.clone(F, C))
          return false;
        auto *H = C.get(L->getHeader());
        IRBuilder<> B(&*H->getFirstNonPHIIt());
        Value *Value = C.get(X.Phi);
        if (!K->isOne())
          Value = B.CreateMul(Value, ConstantInt::get(F.getContext(), *K),
                              "loop.scale");
        if (!Offset.isZero())
          Value = B.CreateAdd(Value, ConstantInt::get(F.getContext(), Offset),
                              "loop.offset");
        auto *Target = C.get(Y.Phi);
        Target->replaceAllUsesWith(Value);
        Target->eraseFromParent();
        if (S.accept(C))
          return true;
        if (S.stopped())
          return false;
      }
  }
  return false;
}
} // namespace neverd::analysis::scalar_recovery
