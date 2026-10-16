//===- RegistrationSEHParentEpilogue.cpp - Parent epilogue bytes ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate the complete physical epilogue before a finally can return
/// directly through its parent frame.

#include "RegistrationSEHControl.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include <set>

namespace neverd {

std::optional<RegistrationSEHControlContract>
getRegistrationSEHParentEpilogue(const BinaryImage &Image,
                                 const LowBlock &Block) {
  if (!Block.Succs.empty() || Block.StartAddr >= Block.EndAddr ||
      Block.EndAddr - Block.StartAddr < 4 ||
      Block.EndAddr - Block.StartAddr > 8)
    return std::nullopt;
  const auto Bytes = readImmutablePE32CodeBytes(
      Image, Block.StartAddr, Block.EndAddr - Block.StartAddr, {});
  if (!Bytes)
    return std::nullopt;
  const size_t Pops = Bytes->size() - 4;
  if ((*Bytes)[Pops] != 0x8b || (*Bytes)[Pops + 1] != 0xe5 ||
      (*Bytes)[Pops + 2] != 0x5d || (*Bytes)[Pops + 3] != 0xc3 ||
      Block.InstructionBoundaries.size() != Pops + 3)
    return std::nullopt;
  std::set<uint8_t> Registers;
  for (size_t I = 0; I != Pops; ++I) {
    const auto Opcode = (*Bytes)[I];
    // ECX may discard a private cookie slot. EAX is the return value; ESP
    // and EBP must retain their coordinates until the canonical final pair.
    if ((Opcode != 0x59 && Opcode != 0x5b && Opcode != 0x5e &&
         Opcode != 0x5f) ||
        !Registers.insert(Opcode).second)
      return std::nullopt;
  }
  va_t Next = Block.StartAddr;
  for (size_t I = 0; I != Block.InstructionBoundaries.size(); ++I) {
    const auto &Insn = Block.InstructionBoundaries[I];
    const bool Return = I == Pops + 2;
    if (Insn.Address != Next || Insn.Size != (I == Pops ? 2 : 1) ||
        Insn.Mode != InstructionMode::Default ||
        Insn.TargetMode != LowInstructionTargetMode::Preserve ||
        Insn.Control != (Return ? LowInstructionControl::Return
                                : LowInstructionControl::None) ||
        Insn.ControlFlags != (Return ? LowInstructionControlFlag::Return
                                     : LowInstructionControlFlag::None) ||
        Insn.Immediate)
      return std::nullopt;
    Next += Insn.Size;
  }
  return RegistrationSEHControlContract{
      RegistrationSEHControlContract::Kind::ParentEpilogue, Block.StartAddr,
      Block.EndAddr, Block.EndAddr - 1};
}

} // namespace neverd
