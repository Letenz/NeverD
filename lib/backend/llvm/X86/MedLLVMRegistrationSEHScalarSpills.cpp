//===- MedLLVMRegistrationSEHScalarSpills.cpp - SEH scalar spills ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MedLLVMRegistrationSEHProof.h"

#include "neverd/Limits.h"

namespace neverd::x86_registration {

bool extendSEHScalarSpills(const MedFunc &Source,
                           const SEHFrameOffsets &Offsets,
                           std::set<SEHFrameValues::Key> &Scalars,
                           size_t &Work) {
  const auto Key = [](const MedVar &V) {
    return SEHFrameValues::Key{V.Kind, V.Id, V.SSAVer};
  };
  const auto UniqueDWORD = [&](const MedVar &V) {
    const auto Definition = Offsets.UniqueDefinitions.find(Key(V));
    return (V.Kind == MedVar::Reg || V.Kind == MedVar::Temp) && V.Id >= 0 &&
           V.SSAVer > 0 && V.Size == 4 &&
           Definition != Offsets.UniqueDefinitions.end() &&
           Definition->second == V.Size;
  };
  for (const auto &Block : Source.Blocks) {
    // No reaching-memory assumption crosses a callback or CFG entry. The
    // scalar seed belongs to an authenticated runtime field; the coordinate
    // belongs to the independent LowIR frame proof.
    std::set<int32_t> Slots;
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return false;
      if (Op.Dead || Op.MemoryOrdering != NdMemoryOrdering::None ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          Op.RegistrationRoot != MedOp::RegistrationRootKind::None ||
          Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
          Op.Opcode == NdOp::INTRINSIC || Op.Opcode == NdOp::ATOMIC_XCHG ||
          Op.Opcode == NdOp::ATOMIC_ADD || Op.Opcode == NdOp::ATOMIC_CMPXCHG ||
          Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR ||
          Op.Opcode == NdOp::INDIR_BR || Op.Opcode == NdOp::RETURN) {
        Slots.clear();
        continue;
      }
      if (Op.Opcode != NdOp::LOAD && Op.Opcode != NdOp::STORE)
        continue;
      if (Op.NumInputs != (Op.Opcode == NdOp::LOAD ? 1 : 2)) {
        Slots.clear();
        continue;
      }
      const auto Offset = Offsets.get(Op.Inputs[0]);
      if (!Offset) {
        Slots.clear();
        continue;
      }
      if (Op.Opcode == NdOp::LOAD) {
        if (Slots.count(*Offset) && UniqueDWORD(Op.Output) &&
            !Offsets.get(Op.Output))
          Scalars.insert(Key(Op.Output));
        continue;
      }
      const auto &Value = Op.Inputs[1];
      if (!Value.Size) {
        Slots.clear();
        continue;
      }
      // A narrow or shifted overwrite destroys the entire DWORD fact, even
      // if the new bytes themselves came from a checked scalar.
      for (auto It = Slots.begin(); It != Slots.end();) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return false;
        if (int64_t(*It) < int64_t(*Offset) + Value.Size &&
            int64_t(*Offset) < int64_t(*It) + 4)
          It = Slots.erase(It);
        else
          ++It;
      }
      if (UniqueDWORD(Value) && Scalars.count(Key(Value)) &&
          !Offsets.get(Value))
        Slots.insert(*Offset);
    }
  }
  return true;
}

} // namespace neverd::x86_registration
