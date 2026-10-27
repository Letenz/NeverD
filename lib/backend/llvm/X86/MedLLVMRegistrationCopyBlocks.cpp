//===- MedLLVMRegistrationCopyBlocks.cpp - PE32 edge identities ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MedLLVMRegistrationCopyBlocks.h"

#include "neverd/Limits.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Instructions.h"

#include <set>

namespace neverd {
bool hasCheckedRegistrationPHICopyEdges(
    const std::map<int, llvm::BasicBlock *> &OriginalBlocks,
    const std::map<llvm::BasicBlock *, std::pair<int, int>> &CopyEdges) {
  if (OriginalBlocks.size() > limits::kMaxRegistrationEHRecords ||
      CopyEdges.size() > limits::kMaxRegistrationEHRecords)
    return false;
  std::set<llvm::BasicBlock *> SourceBlocks;
  for (const auto &[Id, Block] : OriginalBlocks)
    if (!Block || !SourceBlocks.insert(Block).second)
      return false;
  for (const auto &[Block, Edge] : CopyEdges) {
    const auto From = OriginalBlocks.find(Edge.first);
    const auto To = OriginalBlocks.find(Edge.second);
    if (!Block || SourceBlocks.count(Block) || From == OriginalBlocks.end() ||
        To == OriginalBlocks.end() ||
        Block->getParent() != From->second->getParent() ||
        Block->getParent() != To->second->getParent() ||
        Block->getUniquePredecessor() != From->second)
      return false;
    const auto *Branch =
        llvm::dyn_cast_or_null<llvm::UncondBrInst>(Block->getTerminator());
    if (!Branch || Branch->getSuccessor(0) != To->second)
      return false;
  }
  return true;
}
} // namespace neverd
