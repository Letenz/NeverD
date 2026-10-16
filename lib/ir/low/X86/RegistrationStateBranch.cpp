//===- RegistrationStateBranch.cpp - Proven registration CFG choices ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "llvm/ADT/STLExtras.h"

namespace neverd::registration_state {

std::optional<va_t>
RegistrationStateSolver::knownSuccessor(const LowBlock &Block,
                                        const FrameTransfer &Transfer) const {
  if (Block.Succs.size() != 2 || Block.Ops.empty())
    return std::nullopt;
  const auto &Branch = Block.Ops.back();
  if (Branch.Opcode != NdOp::COND_BR || Branch.NumInputs != 2 ||
      !Branch.Inputs[0].isConst())
    return std::nullopt;
  const auto Condition = Transfer.read(Branch.Inputs[1]);
  const auto Taken = Entries.find(Branch.Inputs[0].Offset);
  const auto Fallthrough = Entries.find(Block.EndAddr);
  if (!Condition.Constant || Condition.MayBeFrame || Taken == Entries.end() ||
      Fallthrough == Entries.end() || Taken == Fallthrough ||
      !llvm::is_contained(Block.Succs, Function.Blocks[Taken->second].Id) ||
      !llvm::is_contained(Block.Succs, Function.Blocks[Fallthrough->second].Id))
    return std::nullopt;
  return *Condition.Constant ? Branch.Inputs[0].Offset : Block.EndAddr;
}

} // namespace neverd::registration_state
