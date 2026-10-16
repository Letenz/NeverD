//===- RegistrationLocalUnwindCall.cpp - Physical CRT call identity ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Close a local-unwind occurrence against the immutable CALL and IAT binding.

#include "RegistrationABIPrivate.h"
#include "RegistrationSourceCall.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

namespace neverd {

bool hasCheckedX86LocalUnwindCall(const BinaryImage &Image,
                                  const LowFunc &Function, const LowOp &Op,
                                  const RegistrationSEHLocalUnwind &Call,
                                  size_t &Work) {
  const auto &Runtime = Call.Runtime;
  if (!Runtime.ImportSlot || Op.Seq < 0 || Op.Seq != Call.OpSeq ||
      Op.Addr != Call.Address ||
      Op.Opcode != (Runtime.Indirect ? NdOp::INDIR_CALL : NdOp::CALL) ||
      Op.NumInputs != 1 || !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4 ||
      Op.Inputs[0].Offset != Runtime.Target ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !registration_abi::chargeCalleeWork(Work, 16))
    return false;
  const auto Checked =
      getCheckedX86LocalUnwindContract(Image, Runtime.Target, Runtime.Indirect);
  if (!Checked || *Checked != Runtime)
    return false;
  if (!Runtime.Indirect) {
    const auto *Boundary = registration_state::checkedRegistrationDirectCall(
        Image, Function, Op, Work);
    return Boundary && Boundary->Address + Boundary->Size == Call.EndAddress;
  }
  if (Call.Address > UINT32_MAX - 6 || Call.EndAddress != Call.Address + 6)
    return false;
  const auto Bytes =
      readImmutablePE32CodeBytes(Image, Call.Address, 6, {Call.Address + 2});
  if (!Bytes || (*Bytes)[0] != 0xff || (*Bytes)[1] != 0x15 ||
      readLE<uint32_t>(Bytes->data() + 2) != Runtime.ImportSlot)
    return false;
  for (const auto &Block : Function.Blocks) {
    if (!registration_abi::chargeCalleeWork(Work, Block.Ops.size() + 1))
      return false;
    for (size_t I = 0; I != Block.Ops.size(); ++I) {
      if (&Block.Ops[I] != &Op)
        continue;
      if (auto Error = validateLowInstructionBoundaries(
              Block, LowInstructionBoundaryRequirement::Required)) {
        llvm::consumeError(std::move(Error));
        return false;
      }
      for (const auto &Insn : Block.InstructionBoundaries) {
        if (!registration_abi::chargeCalleeWork(Work, 1))
          return false;
        if (Insn.Address != Op.Addr || I < Insn.FirstOp ||
            I - Insn.FirstOp >= Insn.OpCount)
          continue;
        return Insn.Size == 6 && Insn.Control == LowInstructionControl::Call &&
               Insn.Mode == InstructionMode::Default &&
               Insn.TargetMode == LowInstructionTargetMode::Preserve &&
               !hasLowInstructionControlFlag(
                   Insn.ControlFlags, LowInstructionControlFlag::Conditional) &&
               !hasLowInstructionControlFlag(
                   Insn.ControlFlags,
                   LowInstructionControlFlag::InstructionGuard);
      }
      return false;
    }
  }
  return false;
}

} // namespace neverd
