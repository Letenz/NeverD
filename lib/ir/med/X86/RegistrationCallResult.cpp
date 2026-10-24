//===- RegistrationCallResult.cpp - PE32 source result uses ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Prove unused results from current SSA without inferring a callee prototype.
//===----------------------------------------------------------------------===//

#include "RegistrationSEHRuntimeReturn.h"

#include "neverd/Limits.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/X86/X86RegistrationCallResult.h"

#include <deque>
#include <set>
#include <tuple>

namespace neverd {
bool hasUnusedX86RegistrationCallResult(const MedFunc &Function,
                                        const RegistrationCallFrameEffect &Call,
                                        size_t &Work, bool Indirect,
                                        va_t RuntimeFinally) {
  auto Charge = [&](size_t Amount) {
    if (Work > limits::kMaxRegistrationEHStateWork ||
        Amount > limits::kMaxRegistrationEHStateWork - Work) {
      Work = limits::kMaxRegistrationEHStateWork;
      return false;
    }
    Work += Amount;
    return true;
  };
  if (Function.SkippedSSA || Call.DoesNotReturn || Call.OpSeq < 0)
    return false;
  const MedOp *Root = nullptr;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops) {
      if (!Charge(1))
        return false;
      if (Op.Addr != Call.Address || Op.OriginSeq != Call.OpSeq ||
          (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL))
        continue;
      if (Root || Op.Opcode != (Indirect ? NdOp::INDIR_CALL : NdOp::CALL) ||
          !Op.NumInputs || !Op.Inputs[0].isConst() ||
          Op.Inputs[0].ConstVal != Call.Target || Op.DoesNotReturn ||
          !Op.IntrinsicOutputs.empty())
        return false;
      Root = &Op;
    }
  if (!Root)
    return false;
  const auto Valid = [](const MedVar &V) {
    return V.Size && V.SSAVer >= 0 &&
           (V.Kind == MedVar::Temp || V.Kind == MedVar::Reg ||
            V.Kind == MedVar::Flag);
  };
  if (!Valid(Root->Output))
    return false;
  std::deque<MedVar> Pending{Root->Output};
  std::set<std::tuple<int, int, int>> Seen;
  std::optional<std::set<const MedOp *>> RuntimeReturns;
  while (!Pending.empty()) {
    const auto Value = Pending.front();
    Pending.pop_front();
    if (!Charge(1) || !Valid(Value))
      return false;
    if (!Seen.emplace(Value.Kind, Value.Id, Value.SSAVer).second)
      continue;
    if (Seen.size() >
        ::neverd::limits::ir::
            kRegistrationCallResultHasUnusedX86RegistrationCallResultSeenSizeLimit)
      return false;
    unsigned Definitions = 0;
    for (const auto &Block : Function.Blocks) {
      for (const auto &Phi : Block.Phis) {
        if (!Charge(1 + Phi.Args.size()))
          return false;
        if (Phi.Output == Value) {
          if (Phi.Output.Size != Value.Size)
            return false;
          ++Definitions;
        }
        bool Uses = Phi.ExceptionalEntry && *Phi.ExceptionalEntry == Value;
        if (Uses && Phi.ExceptionalEntry->Size != Value.Size)
          return false;
        for (const auto &[Id, Input] : Phi.Args)
          if (Input == Value) {
            if (Input.Size != Value.Size)
              return false;
            Uses = true;
          }
        if (Uses)
          Pending.push_back(Phi.Output);
      }
      for (const auto &Op : Block.Ops) {
        if (!Charge(1 + Op.NumInputs + Op.IntrinsicOutputs.size()))
          return false;
        if (Op.Output == Value) {
          if (Op.Output.Size != Value.Size)
            return false;
          ++Definitions;
        }
        for (const auto &Output : Op.IntrinsicOutputs)
          if (Output == Value) {
            if (Output.Size != Value.Size)
              return false;
            ++Definitions;
          }
        bool Uses = false;
        for (unsigned I = 0; I != Op.NumInputs; ++I)
          if (Op.Inputs[I] == Value) {
            if (Op.Inputs[I].Size != Value.Size)
              return false;
            Uses = true;
          }
        if (!Uses)
          continue;
        if (Op.Opcode == NdOp::RETURN && RuntimeFinally) {
          if (!RuntimeReturns)
            RuntimeReturns = getRuntimeSEHFinallyReturns(Function, *Root,
                                                         RuntimeFinally, Work);
          if (!RuntimeReturns || !RuntimeReturns->count(&Op))
            return false;
          continue;
        }
        if (Op.NumInputs != 1 || !Valid(Op.Output) ||
            (Op.Opcode != NdOp::COPY && Op.Opcode != NdOp::INT_ZEXT &&
             Op.Opcode != NdOp::INT_SEXT) ||
            (Op.Opcode == NdOp::COPY ? Op.Output.Size != Value.Size
                                     : Op.Output.Size <= Value.Size))
          return false;
        Pending.push_back(Op.Output);
      }
    }
    for (const auto &Info : Function.CallInfos) {
      if (!Charge(1 + Info.Args.size()))
        return false;
      for (const auto &Argument : Info.Args)
        if (Argument == Value)
          return false;
    }
    for (const auto &Clobber : Function.CallClobbers) {
      if (!Charge(1) || Clobber.PreservedInput == Value)
        return false;
      if (Clobber.Value == Value)
        ++Definitions;
    }
    for (const auto &Candidate : Function.StructReturnCandidates) {
      if (!Charge(1 + Candidate.Fields.size()))
        return false;
      for (const auto &Field : Candidate.Fields)
        if (Field == Value)
          return false;
    }
    if (Definitions != 1)
      return false;
  }
  return true;
}
} // namespace neverd
