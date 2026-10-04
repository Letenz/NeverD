//===- LLVMScalarLoopRegions.cpp - Loop regions ---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LLVMScalarLoopRecoveryInternal.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/Utils/Local.h"

namespace neverd::analysis::scalar_recovery {
using namespace llvm;

bool zeroTrip(Search &S, Function &F, DominatorTree &DT, LoopInfo &LI) {
  for (auto *L : LI.getLoopsInPreorder()) {
    if (!S.charge())
      return false;
    auto *Pre = L->getLoopPreheader(), *Latch = L->getLoopLatch();
    auto *Exit = L->getExitBlock(), *Exiting = L->getExitingBlock();
    if (!Pre || !Latch || !Exit || !Exiting ||
        Exit->getSinglePredecessor() != Exiting || !Exit->phis().empty())
      continue;
    auto *Br = dyn_cast<CondBrInst>(Exiting->getTerminator());
    auto *Cmp = Br ? dyn_cast<ICmpInst>(Br->getCondition()) : nullptr;
    if (!Cmp || !Cmp->isEquality())
      continue;
    auto Count = counter(*L, *Cmp);
    if (!Count || Br->getSuccessor(
                      Cmp->getPredicate() == CmpInst::ICMP_EQ ? 0 : 1) != Exit)
      continue;
    auto *Initial =
        dyn_cast<ConstantInt>(Count->Phi->getIncomingValueForBlock(Pre));
    if (!Initial)
      continue;
    SmallVector<std::pair<Instruction *, Value *>, 8> Outputs;
    bool Valid = true;
    for (auto *B : L->blocks())
      for (auto &I : *B) {
        if (!S.charge(1 + I.getNumUses()))
          return false;
        bool Outside = false;
        for (auto *U : I.users())
          if (auto *User = dyn_cast<Instruction>(U))
            Outside |= !L->contains(User->getParent());
        if (!Outside)
          continue;
        Value *Seed = nullptr;
        if (auto *P = dyn_cast<PHINode>(&I);
            P && P->getParent() == L->getHeader())
          Seed = P->getIncomingValueForBlock(Pre);
        for (auto &P : L->getHeader()->phis()) {
          if (!S.charge())
            return false;
          if (P.getNumIncomingValues() != 2) {
            Valid = false;
            break;
          }
          if (P.getIncomingValueForBlock(Latch) != &I)
            continue;
          auto *V = P.getIncomingValueForBlock(Pre);
          if (Seed && Seed != V)
            Valid = false;
          Seed = V;
        }
        if (!Seed)
          Valid = false;
        Outputs.push_back({&I, Seed});
      }
    if (!Valid || Outputs.empty())
      continue;
    auto *Outer = L->getParentLoop();
    auto *OuterPre = Outer ? Outer->getLoopPreheader() : Pre;
    SmallVector<BasicBlock *, 8> Cuts;
    if (OuterPre && OuterPre->phis().empty()) {
      unsigned N = 0;
      for (auto *D = DT.getNode(OuterPre)->getIDom(); D && N++ < 8;
           D = D->getIDom())
        if (!pred_empty(D->getBlock()))
          Cuts.push_back(D->getBlock());
    }
    // A guard alone adds cost and would fail profitability. It is proposed
    // together with removal of a dominating alternative region.
    for (auto *Cut : Cuts) {
      Candidate C;
      if (!S.clone(F, C))
        return false;
      auto *CP = C.get(Pre), *CE = C.get(Exit), *CX = C.get(Exiting);
      SmallPtrSet<BasicBlock *, 16> Inside;
      for (auto *B : L->blocks())
        Inside.insert(C.get(B));
      for (auto [V, Seed] : Outputs) {
        auto *Mapped = C.map(V);
        auto *P = PHINode::Create(V->getType(), 2, "loop.empty",
                                  CE->getFirstNonPHIIt());
        P->addIncoming(Mapped, CX);
        P->addIncoming(C.map(Seed), CP);
        Mapped->replaceUsesWithIf(P, [&](Use &U) {
          auto *I = dyn_cast<Instruction>(U.getUser());
          return I && I != P && !Inside.contains(I->getParent());
        });
      }
      auto *Old = CP->getTerminator();
      IRBuilder<> B(Old);
      auto *Boundary =
          Count->CompareNext
              ? C.map(Count->Bound)
              : B.CreateAdd(C.map(Count->Bound), Count->Step, "loop.limit");
      auto *Empty = B.CreateICmpEQ(Initial, Boundary, "loop.empty.test");
      B.CreateCondBr(Empty, CE, C.get(L->getHeader()));
      Old->eraseFromParent();
      auto *Destination = C.get(OuterPre), *Start = C.get(Cut);
      SmallVector<BasicBlock *, 8> Path;
      for (auto *D = DT.getNode(OuterPre)->getIDom(); D; D = D->getIDom()) {
        Path.push_back(D->getBlock());
        if (D->getBlock() == Cut)
          break;
      }
      // Values computed on the removed dominator chain may still be needed.
      // Relocation is only a proposal: verification checks availability and
      // whole-function proof checks newly executed poison-producing operations.
      auto Insert = Destination->getFirstNonPHIIt();
      for (auto *P : llvm::reverse(Path)) {
        SmallVector<Instruction *, 16> Moving;
        for (auto &I : *C.get(P))
          if (!I.isTerminator() && !isa<PHINode>(I))
            Moving.push_back(&I);
        for (auto *I : Moving)
          I->moveBefore(Insert);
      }
      SmallVector<BasicBlock *, 8> Preds(predecessors(Start));
      for (auto *P : Preds) {
        auto *T = P->getTerminator();
        for (unsigned K = 0; K < T->getNumSuccessors(); ++K)
          if (T->getSuccessor(K) == Start)
            T->setSuccessor(K, Destination);
      }
      removeUnreachableBlocks(*C.Function);
      if (S.accept(C))
        return true;
      if (S.stopped())
        return false;
    }
  }
  return false;
}

bool rotate(Search &S, Function &F, LoopInfo &LI) {
  for (auto *L : LI.getLoopsInPreorder()) {
    if (!S.charge())
      return false;
    auto *Pre = L->getLoopPredecessor(), *Latch = L->getLoopLatch();
    auto *Exit = L->getExitBlock(), *Exiting = L->getExitingBlock();
    if (!Pre || !Latch || !Exit || !Exiting)
      continue;
    auto *Br = dyn_cast<CondBrInst>(Exiting->getTerminator());
    auto *Cmp = Br ? dyn_cast<ICmpInst>(Br->getCondition()) : nullptr;
    if (!Cmp || !Cmp->isEquality())
      continue;
    unsigned ExitIndex = Cmp->getPredicate() == CmpInst::ICMP_EQ ? 0 : 1;
    if (Br->getSuccessor(ExitIndex) != Exit ||
        Br->getSuccessor(1 - ExitIndex) != Latch)
      continue;
    auto *LatchBranch = dyn_cast<UncondBrInst>(Latch->getTerminator());
    if (Exiting != Latch &&
        (!LatchBranch || LatchBranch->getSuccessor() != L->getHeader()))
      continue;
    auto Count = counter(*L, *Cmp);
    if (!Count)
      continue;
    auto HasGuard = [&](BasicBlock *G, BasicBlock *Target) {
      auto *T = G ? dyn_cast<CondBrInst>(G->getTerminator()) : nullptr;
      return T &&
             ((T->getSuccessor(0) == Target && T->getSuccessor(1) == Exit) ||
              (T->getSuccessor(1) == Target && T->getSuccessor(0) == Exit));
    };
    auto *Guard = Pre;
    if (!HasGuard(Guard, L->getHeader())) {
      Guard = Pre->getSinglePredecessor();
      if (!HasGuard(Guard, Pre))
        continue;
    }
    if (pred_size(Exit) != 2)
      continue;
    SmallVector<std::pair<PHINode *, PHINode *>, 8> Outputs;
    bool Valid = true;
    for (auto &P : Exit->phis()) {
      if (P.getNumIncomingValues() != 2 || P.getBasicBlockIndex(Exiting) < 0 ||
          P.getBasicBlockIndex(Guard) < 0) {
        Valid = false;
        break;
      }
      auto *Final = P.getIncomingValueForBlock(Exiting);
      auto *Initial = P.getIncomingValueForBlock(Guard);
      PHINode *Carrier = nullptr;
      for (auto &H : L->getHeader()->phis()) {
        if (!S.charge())
          return false;
        if (H.getNumIncomingValues() != 2 ||
            H.getIncomingValueForBlock(Latch) != Final ||
            H.getIncomingValueForBlock(Pre) != Initial)
          continue;
        if (Carrier)
          Valid = false;
        Carrier = &H;
      }
      if (!Carrier)
        Valid = false;
      Outputs.push_back({&P, Carrier});
    }
    if (!Valid || Outputs.empty())
      continue;
    for (auto *B : L->blocks())
      for (auto &I : *B) {
        if (!S.charge(1 + I.getNumUses()))
          return false;
        for (auto *U : I.users()) {
          auto *User = dyn_cast<Instruction>(U);
          if (!User || L->contains(User->getParent()))
            continue;
          if (!llvm::any_of(Outputs,
                            [&](auto Pair) { return Pair.first == User; }))
            Valid = false;
        }
      }
    if (!Valid)
      continue;
    // Prefer an ordered test for source rendering. Wrapped/signed counters
    // can reject it; equality remains a separately checked fallback.
    for (unsigned Kind = 0; Kind < 3; ++Kind) {
      Candidate C;
      if (!S.clone(F, C))
        return false;
      auto *H = C.get(L->getHeader()), *X = C.get(Exiting), *Q = C.get(Latch);
      auto *G = C.get(Guard), *E = C.get(Exit);
      for (auto [P, Carrier] : Outputs) {
        auto *Output = C.get(P);
        Output->replaceAllUsesWith(C.get(Carrier));
        Output->eraseFromParent();
      }
      auto *Body = H->splitBasicBlock(H->getFirstNonPHIIt(), "loop.body");
      if (Exiting == L->getHeader())
        X = Body;
      auto *Old = X->getTerminator();
      UncondBrInst::Create(Q, Old->getIterator());
      Old->eraseFromParent();
      Old = G->getTerminator();
      UncondBrInst::Create(Guard == Pre ? H : C.get(Pre), Old->getIterator());
      Old->eraseFromParent();
      Old = H->getTerminator();
      IRBuilder<> B(Old);
      auto *Boundary =
          Count->CompareNext
              ? C.map(Count->Bound)
              : B.CreateAdd(C.map(Count->Bound), Count->Step, "loop.limit");
      auto Predicate =
          Kind == 0   ? (Count->Step->isNegative() ? CmpInst::ICMP_UGT
                                                   : CmpInst::ICMP_ULT)
          : Kind == 1 ? (Count->Step->isNegative() ? CmpInst::ICMP_SGT
                                                   : CmpInst::ICMP_SLT)
                      : CmpInst::ICMP_NE;
      auto *More =
          B.CreateICmp(Predicate, C.get(Count->Phi), Boundary, "loop.more");
      B.CreateCondBr(More, Body, E);
      Old->eraseFromParent();
      if (S.accept(C))
        return true;
      if (S.stopped())
        return false;
    }
  }
  return false;
}
} // namespace neverd::analysis::scalar_recovery
