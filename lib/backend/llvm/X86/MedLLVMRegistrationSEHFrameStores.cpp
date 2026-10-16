//===- MedLLVMRegistrationSEHFrameStores.cpp - Saved stack store bounds ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A saved parent ESP may remain a precise frame address inside a finally.

#include "MedLLVMRegistrationSEHProof.h"

#include "neverd/Limits.h"

namespace neverd::x86_registration {

std::optional<std::set<const MedOp *>> getSEHFrameStores(const MedFunc &Source,
                                                         uint64_t EntrySP,
                                                         uint64_t FrameBytes) {
  if (!Source.RegistrationStates || !Source.RegistrationStates->Complete ||
      !Source.RegistrationStates->CallbackStatesComplete ||
      !Source.RegistrationStates->RegistrationLifetimeComplete ||
      Source.SkippedSSA || EntrySP < 4 || EntrySP > FrameBytes ||
      FrameBytes > UINT32_MAX)
    return std::nullopt;
  using Identity = std::pair<va_t, int>;
  using ValueKey = std::tuple<MedVar::VarKind, int, int>;
  const auto Key = [](const MedVar &V) {
    return ValueKey{V.Kind, V.Id, V.SSAVer};
  };
  const auto Valid = [](const MedVar &V) {
    return (V.Kind == MedVar::Reg || V.Kind == MedVar::Temp) && V.Id >= 0 &&
           V.SSAVer > 0 && (V.Size == 4 || V.Size == 8);
  };
  size_t Work = 0;
  std::map<Identity, std::optional<int32_t>> Offsets;
  for (const auto &Value : Source.RegistrationStates->FrameValues) {
    if (++Work > limits::kMaxRegistrationEHStateWork || Value.OpSeq < 0 ||
        !Offsets
             .emplace(Identity{Value.Address, Value.OpSeq},
                      Value.EstablishedFrameOffset)
             .second)
      return std::nullopt;
  }
  struct Definition {
    const MedOp *Operation = nullptr;
    int32_t Offset = 0;
    unsigned Definitions = 0;
  };
  std::map<ValueKey, Definition> Values;
  std::set<Identity> Seen;
  for (const auto &Block : Source.Blocks)
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      const auto Found = Offsets.find({Op.Addr, Op.OriginSeq});
      if (Found == Offsets.end() || !Found->second)
        continue;
      if (!Seen.insert(Found->first).second)
        return std::nullopt;
      if (!Valid(Op.Output) || Op.Dead)
        continue;
      if (!Values.emplace(Key(Op.Output), Definition{&Op, *Found->second, 0})
               .second)
        return std::nullopt;
    }
  const auto Count = [&](const MedVar &Value) {
    const auto Found = Values.find(Key(Value));
    if (Value.Size && Found != Values.end())
      ++Found->second.Definitions;
  };
  for (const auto &Block : Source.Blocks) {
    for (const auto &Phi : Block.Phis) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      Count(Phi.Output);
    }
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      Count(Op.Output);
      for (const auto &Output : Op.IntrinsicOutputs) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return std::nullopt;
        Count(Output);
      }
    }
  }
  for (const auto &Clobber : Source.CallClobbers) {
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return std::nullopt;
    Count(Clobber.Value);
  }
  std::set<const MedOp *> Result;
  for (const auto &Block : Source.Blocks)
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      if (Op.Opcode != NdOp::STORE || Op.NumInputs != 2 || Op.Dead ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          Op.MemoryOrdering != NdMemoryOrdering::None || !Valid(Op.Inputs[0]) ||
          !Op.Inputs[1].Size)
        continue;
      const auto Found = Values.find(Key(Op.Inputs[0]));
      if (Found == Values.end() || Found->second.Definitions != 1 ||
          Found->second.Operation->Output.Size != Op.Inputs[0].Size)
        continue;
      const int64_t Offset = int64_t(EntrySP) - 4 + Found->second.Offset;
      if (Offset >= 0 && uint64_t(Offset) <= FrameBytes &&
          Op.Inputs[1].Size <= FrameBytes - uint64_t(Offset))
        Result.insert(&Op);
    }
  return Result;
}

} // namespace neverd::x86_registration
