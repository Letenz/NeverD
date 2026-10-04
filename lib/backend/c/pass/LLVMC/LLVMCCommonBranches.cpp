//===- LLVMCCommonBranches.cpp - Factor duplicate branch tests ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "LLVMCCommonBranches.h"

#include "neverd/backend/llvm/WindowsEHMetadata.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

namespace neverd::llvmc {

bool factorCommonBranchTests(llvm::Function &Function) {
  using namespace llvm;
  // Bound graph construction, repeated dominance queries, PHI scans and output
  // size independently of source size. Exhaustion retains the remaining CFG.
  constexpr size_t MaxBlocks = 4096;
  constexpr size_t MaxWork = 65536;
  constexpr unsigned MaxJoins = 64;
  if (Function.isDeclaration() || Function.size() > MaxBlocks ||
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
  SmallVector<BasicBlock *, 32> Heads;
  for (auto &Block : Function) {
    if (Block.isEHPad())
      return false;
    Heads.push_back(&Block);
  }
  unsigned Changed = 0;
  for (auto *HeadBlock : Heads) {
    if (!Spend() || Changed == MaxJoins)
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
