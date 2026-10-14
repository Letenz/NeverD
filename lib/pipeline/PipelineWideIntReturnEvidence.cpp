//===- PipelineWideIntReturnEvidence.cpp - Register-pair evidence --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Requires a high-word carrier on every return path, including independent
/// roots, before promoting a 32-bit function to a register-pair result.
///
//===----------------------------------------------------------------------===//

#include "PipelineWideIntReturnDetail.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"

#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {
namespace {

using ValueKey = std::tuple<MedVar::VarKind, int, int>;
ValueKey key(const MedVar &Value) {
  return {Value.Kind, Value.Id, Value.SSAVer};
}

bool isEntryIdentity(const MedOp &Op) {
  return Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
         Op.Output == Op.Inputs[0];
}

class PairReturnEvidence {
  const TargetRegInfo &TRI;
  const int EntryBlock;
  std::map<int, const MedBlock *> Blocks;
  std::map<ValueKey, std::pair<const MedOp *, int>> Definitions;
  std::map<ValueKey, std::pair<const PhiNode *, int>> Phis;

  bool dominatesEdge(int Definition, int Predecessor) const {
    std::vector<int> Pending{Predecessor};
    std::set<int> Seen;
    bool ReachedDefinition = false;
    while (!Pending.empty()) {
      const int Id = Pending.back();
      Pending.pop_back();
      if (Id == Definition) {
        ReachedDefinition = true;
        continue;
      }
      if (!Seen.insert(Id).second)
        continue;
      const auto It = Blocks.find(Id);
      if (It == Blocks.end() || Id == EntryBlock || It->second->Preds.empty() ||
          !It->second->ExceptionalPreds.empty())
        return false;
      Pending.insert(Pending.end(), It->second->Preds.begin(),
                     It->second->Preds.end());
    }
    return ReachedDefinition;
  }

  bool phiHasDefinedInputs(const PhiNode &Phi, int BlockId) const {
    std::vector<std::pair<MedVar, int>> Pending{{Phi.Output, BlockId}};
    std::set<ValueKey> Seen;
    bool HasDefinition = false;
    while (!Pending.empty()) {
      const auto [Value, Predecessor] = Pending.back();
      Pending.pop_back();
      if (Value.Size < TRI.PointerSize)
        return false;
      if (Value.isConst()) {
        HasDefinition = true;
        continue;
      }
      if (const auto It = Phis.find(key(Value)); It != Phis.end()) {
        const auto [InputPhi, Owner] = It->second;
        if (!dominatesEdge(Owner, Predecessor) || InputPhi->Args.empty() ||
            InputPhi->ExceptionalEntry)
          return false;
        if (!Seen.insert(key(Value)).second)
          continue;
        for (const auto &[Pred, Input] : InputPhi->Args)
          Pending.push_back({Input, Pred});
        continue;
      }
      const auto It = Definitions.find(key(Value));
      if (It == Definitions.end() ||
          !dominatesEdge(It->second.second, Predecessor))
        return false;
      const MedOp &Definition = *It->second.first;
      if (isEntryIdentity(Definition) ||
          (Definition.Opcode == NdOp::COPY && Definition.NumInputs == 1 &&
           Definition.Inputs[0].Kind == MedVar::Unspecified))
        return false;
      HasDefinition = true;
    }
    return HasDefinition;
  }

public:
  PairReturnEvidence(const MedFunc &Func, const TargetRegInfo &TRI)
      : TRI(TRI),
        EntryBlock(Func.Blocks.empty() ? -1 : Func.Blocks.front().Id) {
    for (const MedBlock &Block : Func.Blocks) {
      Blocks.emplace(Block.Id, &Block);
      for (const MedOp &Op : Block.Ops)
        if (Op.Output.Size)
          Definitions.emplace(key(Op.Output), std::pair{&Op, Block.Id});
      for (const PhiNode &Phi : Block.Phis)
        Phis.emplace(key(Phi.Output), std::pair{&Phi, Block.Id});
    }
  }

  bool highWordReaches(const MedBlock &ReturnBlock, size_t ReturnIndex) const {
    const MedBlock *Block = &ReturnBlock;
    size_t End = ReturnIndex;
    std::set<int> Seen;
    while (Seen.insert(Block->Id).second) {
      for (size_t I = End; I-- > 0;) {
        const MedOp &Op = Block->Ops[I];
        if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
            !Op.PreservesCallerSaved)
          return false;
        if (Op.Output.Kind != MedVar::Reg ||
            Op.Output.RegOff != TRI.IntReturnReg2 || !Op.Output.Size)
          continue;
        const bool HighSlice = Op.Opcode == NdOp::SUBBYTES &&
                               Op.NumInputs == 2 && Op.Inputs[1].isConst() &&
                               Op.Inputs[1].ConstVal == TRI.PointerSize;
        if (Op.Opcode == NdOp::SUBBYTES && !HighSlice)
          continue;
        if (Op.Output.Size < TRI.PointerSize || isEntryIdentity(Op) ||
            (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
             Op.Inputs[0].Kind == MedVar::Unspecified))
          return false;
        // A pointer calculation can leave its base in the high register.
        // Reading that base to form the low result is not a second result.
        for (size_t J = I + 1; J < End; ++J) {
          const MedOp &Use = Block->Ops[J];
          if (Use.Output.Kind != MedVar::Reg ||
              Use.Output.RegOff != TRI.IntReturnReg)
            continue;
          for (uint8_t K = 0; K < Use.NumInputs; ++K)
            if (Use.Inputs[K] == Op.Output)
              return false;
        }
        return true;
      }
      for (const PhiNode &Phi : Block->Phis)
        if (Phi.Output.Kind == MedVar::Reg &&
            Phi.Output.RegOff == TRI.IntReturnReg2 &&
            Phi.Output.Size >= TRI.PointerSize)
          return phiHasDefinedInputs(Phi, Block->Id);
      if (Block->Preds.size() != 1 || !Block->ExceptionalPreds.empty())
        return false;
      const auto Pred = Blocks.find(Block->Preds.front());
      if (Pred == Blocks.end())
        return false;
      Block = Pred->second;
      End = Block->Ops.size();
    }
    return false;
  }
};

} // namespace

bool returnsRegisterPairI64(
    const MedFunc &Func, const TargetRegInfo &TRI,
    llvm::function_ref<bool(const MedOp &)> TailReturnsPair) {
  if (TRI.PointerSize != 4 || !TRI.IntReturnReg2)
    return false;
  PairReturnEvidence Evidence(Func, TRI);
  bool SawReturn = false;
  for (const MedBlock &Block : Func.Blocks)
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      const MedOp &Return = Block.Ops[I];
      if (Return.Opcode != NdOp::RETURN)
        continue;
      SawReturn = true;
      if (Evidence.highWordReaches(Block, I))
        continue;
      if (!I)
        return false;
      const MedOp &Call = Block.Ops[I - 1];
      if ((Call.Opcode != NdOp::CALL && Call.Opcode != NdOp::INDIR_CALL) ||
          Call.Addr != Return.Addr || !TailReturnsPair(Call))
        return false;
    }
  return SawReturn;
}

bool returnsRegisterPairI64(const MedFunc &Func, const TargetRegInfo &TRI) {
  return returnsRegisterPairI64(Func, TRI, [](const MedOp &) { return false; });
}

} // namespace neverd
