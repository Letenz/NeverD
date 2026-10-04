//===- LLVMCLoopPhases.cpp - Recombine loop expressions ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "LLVMCLoopPhases.h"

#include "neverd/backend/llvm/WindowsEHMetadata.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"

namespace neverd::llvmc {
namespace {
using namespace llvm;

bool supportedOperation(Instruction *I) {
  if (!I || !I->getType()->isIntegerTy() ||
      I->getType()->getIntegerBitWidth() > 128 ||
      I->hasPoisonGeneratingAnnotations())
    return false;
  if (auto *Call = dyn_cast<IntrinsicInst>(I))
    return !Call->hasOperandBundles() && !Call->isConvergent() &&
           !Call->cannotDuplicate() && !Call->cannotMerge() &&
           (Call->getIntrinsicID() == Intrinsic::fshl ||
            Call->getIntrinsicID() == Intrinsic::fshr);
  if (I->mayHaveSideEffects() || I->mayReadFromMemory())
    return false;
  switch (I->getOpcode()) {
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
  case Instruction::Trunc:
  case Instruction::ZExt:
  case Instruction::SExt:
    return true;
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
    if (auto *Amount = dyn_cast<ConstantInt>(I->getOperand(1)))
      return Amount->getValue().ult(I->getType()->getIntegerBitWidth());
    return false;
  default:
    return false;
  }
}
} // namespace

bool factorLoopPhiExpressions(llvm::Function &Function) {
  using namespace llvm;
  constexpr size_t MaxWork = 65536;
  constexpr unsigned MaxChanges = 64;
  if (Function.isDeclaration() || Function.size() > 4096 ||
      Function.hasPersonalityFn() ||
      Function.getMetadata(windows_eh_md::FunctionAttachment) ||
      Function.getMetadata(windows_eh_md::NativeAttachment) ||
      Function.getParent()->getNamedMetadata(windows_eh_md::FunctionTable))
    return false;
  size_t Work = MaxWork;
  auto Spend = [&](size_t Amount = 1) {
    if (Amount > Work) {
      Work = 0;
      return false;
    }
    Work -= Amount;
    return true;
  };
  for (auto &Block : Function) {
    if (Block.isEHPad() || !Spend(Block.size()))
      return false;
  }
  DominatorTree Dominators(Function);
  LoopInfo Loops(Dominators);
  SmallVector<PHINode *, 32> Pending;
  for (auto *Loop : Loops.getLoopsInPreorder())
    if (Loop->getLoopPreheader())
      for (auto &Phi : Loop->getHeader()->phis())
        Pending.push_back(&Phi);

  unsigned Changed = 0;
  for (size_t Next = 0; Next < Pending.size() && Changed < MaxChanges; ++Next) {
    if (!Spend())
      break;
    auto *Phi = Pending[Next];
    if (!Phi->getType()->isIntegerTy() ||
        Phi->getType()->getIntegerBitWidth() > 128 ||
        Phi->getNumIncomingValues() < 2 || Phi->getNumIncomingValues() > 32)
      continue;
    auto *Loop = Loops.getLoopFor(Phi->getParent());
    SmallVector<Instruction *, 4> Roots;
    SmallVector<BasicBlock *, 4> Edges;
    SmallPtrSet<BasicBlock *, 4> Seen;
    bool Valid = true;
    for (unsigned K = 0; K < Phi->getNumIncomingValues(); ++K) {
      auto *Root = dyn_cast<Instruction>(Phi->getIncomingValue(K));
      auto *Edge = Phi->getIncomingBlock(K);
      if (!Spend() || !Seen.insert(Edge).second || !supportedOperation(Root) ||
          !Root->hasOneUse() ||
          (!Roots.empty() && !Roots.front()->isSameOperationAs(Root))) {
        Valid = false;
        break;
      }
      if (auto *Call = dyn_cast<CallBase>(Root))
        if (!Roots.empty() &&
            Call->getCalledOperand() !=
                cast<CallBase>(Roots.front())->getCalledOperand()) {
          Valid = false;
          break;
        }
      Roots.push_back(Root);
      Edges.push_back(Edge);
    }
    if (!Valid)
      continue;

    struct OperandPlan {
      Value *Existing = nullptr;
      SmallVector<Value *, 4> Incoming;
    };
    SmallVector<OperandPlan, 3> Plans;
    auto *Template = Roots.front();
    unsigned Count = isa<CallBase>(Template)
                         ? cast<CallBase>(Template)->arg_size()
                         : Template->getNumOperands();
    for (unsigned O = 0; O < Count && Valid; ++O) {
      OperandPlan Plan;
      if (!Spend(Roots.size())) {
        Valid = false;
        break;
      }
      for (auto *Root : Roots) {
        auto *V = Root->getOperand(O);
        if (!V->getType()->isIntegerTy() ||
            V->getType()->getIntegerBitWidth() > 128 || isa<UndefValue>(V) ||
            isa<PoisonValue>(V)) {
          Valid = false;
          break;
        }
        Plan.Incoming.push_back(V);
      }
      if (!Valid)
        break;
      auto *Common = Plan.Incoming.front();
      // Equal SSA names on backedges can still denote the previous iteration.
      // Only an invariant available before the loop may bypass an operand PHI.
      if (llvm::all_of(Plan.Incoming, [&](Value *V) { return V == Common; }) &&
          Loop->isLoopInvariant(Common) &&
          (!isa<Instruction>(Common) ||
           Dominators.dominates(cast<Instruction>(Common),
                                Loop->getLoopPreheader()->getTerminator()))) {
        Plan.Existing = Common;
      } else {
        for (auto &Other : Phi->getParent()->phis()) {
          if (!Spend(1 + Other.getNumIncomingValues() * Edges.size())) {
            Valid = false;
            break;
          }
          if (&Other == Phi || Other.getType() != Common->getType() ||
              Other.getNumIncomingValues() != Edges.size())
            continue;
          bool Match = true;
          for (unsigned K = 0; K < Edges.size(); ++K)
            Match &=
                Other.getIncomingValueForBlock(Edges[K]) == Plan.Incoming[K];
          if (Match) {
            Plan.Existing = &Other;
            break;
          }
        }
      }
      Plans.push_back(std::move(Plan));
    }
    // Planning is transactional: budget/refusal leaves the entire PHI intact.
    if (!Valid || !Spend(Count * (Count + Edges.size()) + Roots.size()))
      continue;
    SmallVector<Value *, 3> Operands;
    for (unsigned O = 0; O < Plans.size(); ++O) {
      auto &Plan = Plans[O];
      Value *Operand = Plan.Existing;
      if (!Operand)
        for (unsigned Earlier = 0; Earlier < O; ++Earlier)
          if (Plans[Earlier].Incoming == Plan.Incoming) {
            Operand = Operands[Earlier];
            break;
          }
      if (!Operand) {
        auto *Input =
            PHINode::Create(Plan.Incoming.front()->getType(), Edges.size(),
                            Phi->getName() + ".input", Phi->getIterator());
        for (unsigned K = 0; K < Edges.size(); ++K)
          Input->addIncoming(Plan.Incoming[K], Edges[K]);
        Operand = Input;
        Pending.push_back(Input);
      }
      Operands.push_back(Operand);
    }
    auto *Result = Template->clone();
    for (unsigned O = 0; O < Operands.size(); ++O)
      Result->setOperand(O, Operands[O]);
    Result->insertInto(Phi->getParent(), Phi->getParent()->getFirstNonPHIIt());
    Result->takeName(Phi);
    Phi->replaceAllUsesWith(Result);
    Phi->eraseFromParent();
    // All roots had exactly this PHI as their sole use. Removing them now
    // exposes their operand phases to later bounded worklist entries.
    for (auto *Root : Roots)
      Root->eraseFromParent();
    ++Changed;
  }
  return Changed != 0;
}

} // namespace neverd::llvmc
