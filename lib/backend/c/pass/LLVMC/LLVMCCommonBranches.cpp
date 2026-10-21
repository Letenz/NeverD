//===- LLVMCCommonBranches.cpp - Factor duplicate branch tests ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "LLVMCCommonBranches.h"

#include "neverd/backend/Limits.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

namespace neverd::llvmc {

bool distributeReturnEdges(llvm::Function &Function) {
  using namespace llvm;
  if (Function.isDeclaration() ||
      Function.size() >
          ::neverd::limits::backend::kReturnEdgeDistributionMaxBlocks ||
      Function.hasPersonalityFn() ||
      Function.getMetadata(windows_eh_md::FunctionAttachment) ||
      Function.getMetadata(windows_eh_md::NativeAttachment) ||
      Function.getParent()->getNamedMetadata(windows_eh_md::FunctionTable))
    return false;
  SmallVector<BasicBlock *, 8> Returns;
  for (auto &Block : Function) {
    if (Block.isEHPad())
      return false;
    if (isa<ReturnInst>(Block.getTerminator()))
      Returns.push_back(&Block);
  }
  size_t Work = ::neverd::limits::backend::kLLVMCCommonBranchesMaxEdges;
  bool Changed = false;
  for (auto *Block : Returns) {
    auto *Return = cast<ReturnInst>(Block->getTerminator());
    auto *Value = Return->getReturnValue();
    auto *Phi = dyn_cast_or_null<PHINode>(Value);
    if (Phi && Phi->getParent() != Block)
      Phi = nullptr;
    if (Block->hasAddressTaken() || pred_empty(Block) ||
        Block->size() != (Phi ? 2u : 1u) || (Phi && !Phi->hasOneUse()))
      continue;
    SmallPtrSet<BasicBlock *, 8> Seen;
    SmallVector<BasicBlock *, 8> Predecessors;
    bool Valid = true;
    for (auto *Pred : predecessors(Block)) {
      if (!Work) {
        Valid = false;
        break;
      }
      --Work;
      if (!isa<UncondBrInst, CondBrInst>(Pred->getTerminator())) {
        Valid = false;
        break;
      }
      if (Seen.insert(Pred).second)
        Predecessors.push_back(Pred);
    }
    if (!Valid)
      continue;
    // The return block has no effects to duplicate. Its selected PHI operand
    // already dominates the incoming edge, including when it is poison on an
    // untaken edge. Calls, ordered loads and cleanup work remain in place.
    for (auto *Pred : Predecessors) {
      auto *Incoming = Phi ? Phi->getIncomingValueForBlock(Pred) : Value;
      auto *Branch = Pred->getTerminator();
      // A return without a PHI gives the statement writer one common
      // exit for short-circuit conditions. Splitting those conditional edges
      // destroys that identity and can insert a return block before the
      // ordinary fallthrough. Only PHI substitution needs a private edge.
      if (!Phi && isa<CondBrInst>(Branch))
        continue;
      ReturnInst *NewReturn;
      if (isa<UncondBrInst>(Branch)) {
        NewReturn = ReturnInst::Create(Function.getContext(), Incoming,
                                       Branch->getIterator());
        Branch->eraseFromParent();
      } else {
        auto *Edge = BasicBlock::Create(Function.getContext(), "return.edge",
                                        &Function, Pred->getNextNode());
        NewReturn = ReturnInst::Create(Function.getContext(), Incoming, Edge);
        for (unsigned I = 0; I < Branch->getNumSuccessors(); ++I)
          if (Branch->getSuccessor(I) == Block)
            Branch->setSuccessor(I, Edge);
      }
      NewReturn->copyMetadata(*Return);
      NewReturn->setDebugLoc(Return->getDebugLoc());
      Changed = true;
    }
    if (pred_empty(Block)) {
      Block->dropAllReferences();
      Block->eraseFromParent();
    }
  }
  return Changed;
}

bool factorCommonBranchTests(llvm::Function &Function) {
  using namespace llvm;
  if (Function.isDeclaration() ||
      Function.size() >
          ::neverd::limits::backend::kCommonBranchFactoringMaxBlocks ||
      Function.hasPersonalityFn() ||
      Function.getMetadata(windows_eh_md::FunctionAttachment) ||
      Function.getMetadata(windows_eh_md::NativeAttachment) ||
      Function.getParent()->getNamedMetadata(windows_eh_md::FunctionTable))
    return false;
  size_t Work = ::neverd::limits::backend::
      kLLVMCCommonBranchesFactorCommonBranchTestsMaxWork;
  auto Spend = [&](size_t Amount = 1) {
    if (Amount > Work) {
      Work = 0;
      return false;
    }
    Work -= Amount;
    return true;
  };
  SmallVector<BasicBlock *, 32> Heads;
  for (auto &Block : Function) {
    if (Block.isEHPad())
      return false;
    Heads.push_back(&Block);
  }
  unsigned Changed = 0;
  for (auto *HeadBlock : Heads) {
    if (!Spend() ||
        Changed == ::neverd::limits::backend::kLLVMCCommonBranchesMaxJoins)
      break;
    auto *Head = dyn_cast<CondBrInst>(HeadBlock->getTerminator());
    if (!Head)
      continue;
    auto *Left = Head->getSuccessor(0);
    auto *Right = Head->getSuccessor(1);
    if (Left == Right || Left == HeadBlock || Right == HeadBlock ||
        Left->getSinglePredecessor() != HeadBlock ||
        Right->getSinglePredecessor() != HeadBlock)
      continue;
    auto *LeftBranch = dyn_cast<CondBrInst>(Left->getTerminator());
    auto *RightBranch = dyn_cast<CondBrInst>(Right->getTerminator());
    if (!LeftBranch || !RightBranch ||
        LeftBranch->getSuccessor(0) == LeftBranch->getSuccessor(1) ||
        LeftBranch->getSuccessor(0) != RightBranch->getSuccessor(0) ||
        LeftBranch->getSuccessor(1) != RightBranch->getSuccessor(1))
      continue;
    auto *LeftCompare = dyn_cast<ICmpInst>(LeftBranch->getCondition());
    auto *RightCompare = dyn_cast<ICmpInst>(RightBranch->getCondition());
    if (!LeftCompare || !RightCompare ||
        !LeftCompare->isIdenticalTo(RightCompare) ||
        !LeftCompare->getOperand(0)->getType()->isIntegerTy() ||
        !Spend(Function.size()))
      continue;
    DominatorTree Dominators(Function);
    bool Valid = true;
    for (auto *Operand : LeftCompare->operand_values())
      if (auto *Instruction = dyn_cast<llvm::Instruction>(Operand))
        Valid &= Dominators.dominates(Instruction, Head);
    if (!Valid)
      continue;

    struct EdgeValues {
      PHINode *Target;
      Value *Left;
      Value *Right;
    };
    SmallVector<EdgeValues, 8> Edges;
    auto *TrueBlock = LeftBranch->getSuccessor(0);
    auto *FalseBlock = LeftBranch->getSuccessor(1);
    for (auto *Successor : {TrueBlock, FalseBlock}) {
      for (auto &Phi : Successor->phis()) {
        // Reserve incoming-edge lookup, mutation and new PHI work together.
        if (!Spend(8 + 8 * size_t(Phi.getNumIncomingValues()))) {
          Valid = false;
          break;
        }
        int L = Phi.getBasicBlockIndex(Left);
        int R = Phi.getBasicBlockIndex(Right);
        if (L < 0 || R < 0) {
          Valid = false;
          break;
        }
        Edges.push_back(
            {&Phi, Phi.getIncomingValue(L), Phi.getIncomingValue(R)});
      }
      if (!Valid)
        break;
    }
    if (!Valid)
      continue;

    // Keep both bodies in place, including calls and ordered memory accesses.
    // Only the identical integer test moves, after the selected body. Join
    // PHIs select each successor's old edge values before that test; unused
    // poison values on another successor do not become control dependencies.
    auto *Join = BasicBlock::Create(Function.getContext(), "common.branch.test",
                                    &Function, TrueBlock);
    DenseMap<std::pair<Value *, Value *>, PHINode *> Merged;
    for (const auto &Edge : Edges) {
      Value *Incoming = Edge.Left;
      if (Edge.Left != Edge.Right) {
        auto Key = std::make_pair(Edge.Left, Edge.Right);
        auto [It, Inserted] = Merged.try_emplace(Key, nullptr);
        if (Inserted) {
          It->second =
              PHINode::Create(Edge.Target->getType(), 2,
                              Edge.Target->getName() + ".joined", Join);
          It->second->addIncoming(Edge.Left, Left);
          It->second->addIncoming(Edge.Right, Right);
        }
        Incoming = It->second;
      }
      Edge.Target->removeIncomingValue(Left, false);
      Edge.Target->removeIncomingValue(Right, false);
      Edge.Target->addIncoming(Incoming, Join);
    }
    auto *Compare = LeftCompare->clone();
    Compare->insertInto(Join, Join->end());
    IRBuilder<>(Join).CreateCondBr(Compare, TrueBlock, FalseBlock);
    IRBuilder<>(LeftBranch).CreateBr(Join);
    IRBuilder<>(RightBranch).CreateBr(Join);
    LeftBranch->eraseFromParent();
    RightBranch->eraseFromParent();
    // Avoid exposing a second layer of source copies when the two old arms
    // were a successor's only predecessors.
    for (const auto &Edge : Edges) {
      if (Edge.Target->getNumIncomingValues() != 1)
        continue;
      auto *Incoming = Edge.Target->getIncomingValue(0);
      if (Incoming == Edge.Target)
        continue;
      Edge.Target->replaceAllUsesWith(Incoming);
      Edge.Target->eraseFromParent();
    }
    ++Changed;
  }
  return Changed != 0;
}

} // namespace neverd::llvmc
