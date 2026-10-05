//===- LLVMScalarLoopPredicates.cpp - Loop condition proposals -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LLVMScalarLoopRecoveryInternal.h"

#include "llvm/IR/IRBuilder.h"

namespace neverd::analysis::scalar_recovery {
using namespace llvm;
namespace {
struct EntryGuard {
  ICmpInst::Predicate Predicate;
  ConstantInt *Seed;
  Value *Bound;
};
std::optional<EntryGuard> entryGuard(Value *V) {
  auto *Cmp = dyn_cast<ICmpInst>(V);
  if (!Cmp)
    return {};
  auto *Seed = dyn_cast<ConstantInt>(Cmp->getOperand(0));
  if (Seed && !isa<ConstantInt>(Cmp->getOperand(1)))
    return EntryGuard{Cmp->getPredicate(), Seed, Cmp->getOperand(1)};
  Seed = dyn_cast<ConstantInt>(Cmp->getOperand(1));
  if (Seed && !isa<ConstantInt>(Cmp->getOperand(0)))
    return EntryGuard{ICmpInst::getSwappedPredicate(Cmp->getPredicate()), Seed,
                      Cmp->getOperand(0)};
  return {};
}
} // namespace

bool predicates(Search &S, Function &F, DominatorTree &DT, LoopInfo &LI) {
  for (auto *L : LI.getLoopsInPreorder()) {
    if (!S.charge())
      return false;
    auto *Branch = dyn_cast<CondBrInst>(L->getHeader()->getTerminator());
    auto *Guard = Branch ? dyn_cast<PHINode>(Branch->getCondition()) : nullptr;
    if (!Guard || Guard->getParent() != L->getHeader())
      continue;
    std::optional<EntryGuard> First;
    bool Valid = true;
    for (unsigned I = 0; I < Guard->getNumIncomingValues(); ++I) {
      if (!S.charge())
        return false;
      if (L->contains(Guard->getIncomingBlock(I)))
        continue;
      auto E = entryGuard(Guard->getIncomingValue(I));
      if (!E || !L->isLoopInvariant(E->Bound) ||
          !DT.dominates(E->Bound, Guard) ||
          (First &&
           (E->Bound != First->Bound || E->Predicate != First->Predicate))) {
        Valid = false;
        break;
      }
      First = E;
    }
    if (!Valid || !First)
      continue;
    unsigned BW = First->Bound->getType()->getIntegerBitWidth();
    for (auto &P : L->getHeader()->phis()) {
      if (!S.charge())
        return false;
      if (&P == Guard || !P.getType()->isIntegerTy())
        continue;
      unsigned Width = P.getType()->getIntegerBitWidth();
      if (Width < BW || Width > 64 || Width == 1)
        continue;
      for (unsigned Signed = 0; Signed != (Width == BW ? 1u : 2u); ++Signed) {
        bool Matches = true;
        for (unsigned I = 0; I < Guard->getNumIncomingValues(); ++I) {
          if (!S.charge())
            return false;
          auto *Edge = Guard->getIncomingBlock(I);
          if (L->contains(Edge))
            continue;
          auto E = entryGuard(Guard->getIncomingValue(I));
          // Account for the linear incoming-edge lookup as well as the visit.
          if (!S.charge(P.getNumIncomingValues()))
            return false;
          int Index = P.getBasicBlockIndex(Edge);
          auto *Seed = Index < 0
                           ? nullptr
                           : dyn_cast<ConstantInt>(P.getIncomingValue(Index));
          if (!E || !Seed ||
              Seed->getValue() != (Signed ? E->Seed->getValue().sext(Width)
                                          : E->Seed->getValue().zext(Width))) {
            Matches = false;
            break;
          }
        }
        if (!Matches)
          continue;
        Candidate C;
        if (!S.clone(F, C) || !S.charge(3))
          return false;
        auto *Old = C.get(Guard);
        IRBuilder<> B(&*Old->getParent()->getFirstInsertionPt());
        auto *Bound = C.map(First->Bound);
        if (Width != BW)
          Bound = Signed ? B.CreateSExt(Bound, P.getType())
                         : B.CreateZExt(Bound, P.getType());
        auto *Condition =
            B.CreateICmp(First->Predicate, C.get(&P), Bound, "recovery.guard");
        Old->replaceAllUsesWith(Condition);
        Old->eraseFromParent();
        // Entry agreement only nominates this comparison. The complete source
        // proof must preserve every backedge, definedness and termination.
        if (S.accept(C))
          return true;
        if (S.stopped())
          return false;
      }
    }
  }
  return false;
}
} // namespace neverd::analysis::scalar_recovery
