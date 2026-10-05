//===- LLVMScalarLoopUnpeeling.cpp - Loop unpeeling -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LLVMScalarLoopRecoveryInternal.h"

#include "llvm/ADT/SetVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/Transforms/Utils/Local.h"

namespace neverd::analysis::scalar_recovery {
using namespace llvm;
namespace {
constexpr unsigned MaxSlots = 4;
constexpr unsigned MaxValues = 12;
constexpr unsigned MaxDepth = 6;
constexpr unsigned MaxVisits = 128;
constexpr unsigned MaxCuts = 8;

struct Slot {
  PHINode *Phi;
  BasicBlock *Edge;
  SmallVector<Value *, MaxValues> Choices;
};

bool addSlot(Search &S, Slot &Slot, DominatorTree &DT) {
  auto *Root = Slot.Phi->getIncomingValueForBlock(Slot.Edge);
  SmallVector<std::pair<Value *, unsigned>, 32> Work{{Root, 0}};
  SmallPtrSet<Value *, 32> Seen;
  for (size_t N = 0;
       N < Work.size() && N < MaxVisits && Slot.Choices.size() < MaxValues;
       ++N) {
    if (!S.charge())
      return false;
    auto [V, Depth] = Work[N];
    if (!Seen.insert(V).second)
      continue;
    auto *I = dyn_cast<Instruction>(V);
    if (V->getType() == Slot.Phi->getType() &&
        (!I || DT.dominates(I, Slot.Edge->getTerminator())))
      Slot.Choices.push_back(V);
    if (!I || isa<PHINode>(I) || Depth >= MaxDepth)
      continue;
    for (auto *O : I->operand_values())
      Work.push_back({O, Depth + 1});
  }
  return !Slot.Choices.empty();
}
} // namespace

bool unpeel(Search &S, Function &F, DominatorTree &DT, LoopInfo &LI) {
  bool Found = false;
  for (auto *L : LI.getLoopsInPreorder()) {
    if (!S.charge())
      return false;
    auto *Pre = L->getLoopPredecessor(), *Latch = L->getLoopLatch();
    auto *Exit = L->getExitBlock();
    if (!Pre || !Latch || !Exit)
      continue;
    auto *Gate = dyn_cast<CondBrInst>(Pre->getTerminator());
    bool Direct = Gate && ((Gate->getSuccessor(0) == L->getHeader() &&
                            Gate->getSuccessor(1) == Exit) ||
                           (Gate->getSuccessor(1) == L->getHeader() &&
                            Gate->getSuccessor(0) == Exit));
    if (!Direct && (L->getLoopPreheader() != Pre || !Pre->phis().empty()))
      continue;
    SmallVector<std::pair<PHINode *, ConstantInt *>, 4> Rewind;
    SmallVector<PHINode *, 4> Unknown;
    bool Valid = true;
    for (auto &P : L->getHeader()->phis()) {
      if (!S.charge())
        return false;
      if (P.getNumIncomingValues() != 2) {
        Valid = false;
        break;
      }
      auto *Initial = dyn_cast<ConstantInt>(P.getIncomingValueForBlock(Pre));
      if (!Initial) {
        Unknown.push_back(&P);
        continue;
      }
      auto *Update =
          dyn_cast<BinaryOperator>(P.getIncomingValueForBlock(Latch));
      if (!Update || Update->getOpcode() != Instruction::Add) {
        Valid = false;
        break;
      }
      Value *A = Update->getOperand(0), *B = Update->getOperand(1);
      if (B == &P)
        std::swap(A, B);
      auto *Step = dyn_cast<ConstantInt>(B);
      if (A != &P || !Step || Step->isZero()) {
        Valid = false;
        break;
      }
      Rewind.push_back(
          {&P, ConstantInt::get(F.getContext(),
                                Initial->getValue() - Step->getValue())});
    }
    if (!Valid || Rewind.empty() || Unknown.empty() ||
        Unknown.size() > MaxSlots)
      continue;
    SmallVector<BasicBlock *, MaxCuts> Cuts;
    if (Direct) {
      Cuts.push_back(nullptr);
    } else {
      unsigned N = 0;
      for (auto *D = DT.getNode(Pre)->getIDom(); D && N++ < MaxCuts;
           D = D->getIDom())
        if (!pred_empty(D->getBlock()))
          Cuts.push_back(D->getBlock());
    }
    for (auto *Cut : Cuts) {
      SmallVector<Slot, MaxSlots> Slots;
      for (auto *P : Unknown) {
        auto *Seed = P->getIncomingValueForBlock(Pre);
        auto *Carrier = dyn_cast<PHINode>(Seed);
        if (Direct && Carrier && Carrier->getParent() == Pre) {
          for (auto *Edge : Carrier->blocks())
            Slots.push_back({Carrier, Edge, {}});
        } else {
          Slots.push_back({P, Pre, {}});
        }
      }
      if (Slots.size() > MaxSlots)
        continue;
      for (auto &Slot : Slots) {
        if (Direct) {
          Valid &= addSlot(S, Slot, DT);
        } else {
          // Prefix removal can leave values defined inside the removed region.
          // Nominate only input/earlier dominating values, never a saved
          // address.
          if (!S.charge(F.arg_size()))
            return false;
          // Reserve proposal slots for inputs used by the current body. The
          // original signature and the proof over every input stay unchanged.
          for (auto &A : F.args())
            if (!A.use_empty() && A.getType() == Slot.Phi->getType() &&
                Slot.Choices.size() < MaxValues)
              Slot.Choices.push_back(&A);
          if (Slot.Choices.size() < MaxValues)
            Slot.Choices.push_back(ConstantInt::get(Slot.Phi->getType(), 0));
          for (auto &B : F) {
            if (!S.charge())
              return false;
            if (&B == Cut || !DT.dominates(&B, Cut))
              continue;
            if (!S.charge(B.size()))
              return false;
            for (auto &I : B)
              if (I.getType() == Slot.Phi->getType() &&
                  Slot.Choices.size() < MaxValues)
                Slot.Choices.push_back(&I);
          }
        }
      }
      if (!Valid || S.stopped())
        continue;
      uint64_t Combinations = 1;
      for (auto &Slot : Slots)
        Combinations *= Slot.Choices.size();
      for (uint64_t N = 0; N < Combinations; ++N) {
        Candidate C;
        if (!S.clone(F, C))
          return false;
        auto *CP = C.get(Pre);
        for (auto [P, V] : Rewind)
          C.get(P)->setIncomingValueForBlock(CP, V);
        uint64_t Choice = N;
        for (auto &Slot : Slots) {
          auto *V = Slot.Choices[Choice % Slot.Choices.size()];
          Choice /= Slot.Choices.size();
          C.get(Slot.Phi)->setIncomingValueForBlock(C.get(Slot.Edge), C.map(V));
        }
        if (Direct) {
          C.get(Exit)->removePredecessor(CP);
          auto *Old = CP->getTerminator();
          UncondBrInst::Create(C.get(L->getHeader()), Old->getIterator());
          Old->eraseFromParent();
        } else {
          auto *Start = C.get(Cut);
          SmallVector<BasicBlock *, 8> Preds(predecessors(Start));
          for (auto *P : Preds) {
            auto *T = P->getTerminator();
            for (unsigned K = 0; K < T->getNumSuccessors(); ++K)
              if (T->getSuccessor(K) == Start)
                T->setSuccessor(K, CP);
          }
          removeUnreachableBlocks(*C.Function);
        }
        if (S.accept(C))
          Found = true;
        if (S.stopped())
          return false;
      }
    }
  }
  return Found;
}
} // namespace neverd::analysis::scalar_recovery
