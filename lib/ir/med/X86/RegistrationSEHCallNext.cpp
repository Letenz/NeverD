//===- RegistrationSEHCallNext.cpp - Nonreturning adjacent finally calls -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Separates an adjacent callback entry from a call whose checked invocation
/// always abandons its return PC through a nonlocal continuation.

#include "RegistrationRoots.h"

#include "neverd/Limits.h"
#include "neverd/ir/med/X86RegistrationSEHCall.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>

namespace neverd::x86_registration {

void RegistrationRoots::disconnectAdjacentSEHCalls(MedFunc &Func) const {
  const auto Returns = getRegistrationSEHReturns(Func);
  const auto Nonlocal = getRegistrationSEHNonlocalCalls(Func);
  if (!Returns || !Nonlocal || Nonlocal->empty())
    return;
  const auto &Controls = Func.RegistrationStates->SEHControls;
  if (Controls.size() > limits::kMaxRegistrationEHStateWork ||
      Func.Blocks.size() > limits::kMaxRegistrationEHRecords)
    return;
  std::map<int, MedBlock *> Blocks;
  for (auto &Block : Func.Blocks)
    if (!Blocks.emplace(Block.Id, &Block).second)
      return;
  size_t Work = 0;
  std::vector<std::pair<MedBlock *, MedBlock *>> Edges;
  for (const auto &[Identity, Target] : *Nonlocal) {
    const auto [Id, Address, Seq] = Identity;
    if (Returns->ReturningCalls.count(Identity) || !Blocks.count(Id))
      continue;
    auto &Block = *Blocks.at(Id);
    if (Block.EndAddr != Target || Block.Ops.empty() ||
        Block.Succs.size() != 1 || !Blocks.count(Block.Succs.front()))
      continue;
    auto &Callback = *Blocks.at(Block.Succs.front());
    const auto &Call = Block.Ops.back();
    if (Callback.StartAddr != Target || Call.Addr != Address ||
        Call.OriginSeq != Seq || Call.Opcode != NdOp::CALL ||
        Callback.Preds != std::vector<int>{Id})
      continue;
    if (Controls.size() > limits::kMaxRegistrationEHStateWork - Work)
      return;
    Work += Controls.size();
    if (llvm::count_if(Controls, [&](const auto &Control) {
          return Control.Control ==
                     RegistrationSEHControlContract::Kind::NearCall &&
                 Control.Address == Address && Control.EndAddress == Target &&
                 Control.Target == Target;
        }) != 1)
      continue;
    Edges.emplace_back(&Block, &Callback);
  }
  for (const auto &[Block, Callback] : Edges) {
    Block->Ops.back().DoesNotReturn = true;
    Block->Succs.clear();
    std::erase(Callback->Preds, Block->Id);
  }
}

} // namespace neverd::x86_registration
