//===- RegistrationJumpRuntime.cpp - Checked PE32 jump runtime binding ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Match the original runtime import and physical veneer for nonlocal jumps.

#include "RegistrationJumpRuntime.h"

#include "RegistrationABIPrivate.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/STLExtras.h"

namespace neverd::registration_state {

std::optional<JumpRuntimeBinding>
checkedJumpRuntime(const BinaryImage &Image, va_t Target, bool Indirect) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || Target > UINT32_MAX)
    return std::nullopt;
  va_t Slot = Target;
  if (!Indirect) {
    if (Target > UINT32_MAX - 6)
      return std::nullopt;
    const auto Bytes =
        readImmutablePE32CodeBytes(Image, Target, 6, {Target + 2});
    if (!Bytes || (*Bytes)[0] != 0xff || (*Bytes)[1] != 0x25)
      return std::nullopt;
    Slot = readLE<uint32_t>(Bytes->data() + 2);
  }
  const auto *Imported = Image.findImportAt(Slot);
  if (!Imported || Imported->IATAddr != Slot || Imported->IsDelayImport ||
      !isFileBackedPEImportSlot(Image, Slot))
    return std::nullopt;
  const llvm::StringRef Module = Imported->Module;
  if (!Module.equals_insensitive("msvcrt.dll") &&
      !Module.equals_insensitive("vcruntime140.dll") &&
      !Module.equals_insensitive("vcruntime140d.dll"))
    return std::nullopt;
  JumpRuntimeBinding Binding;
  if (Imported->Name == "_setjmp3")
    Binding.Routine = JumpRuntimeBinding::Kind::Capture;
  else if (Imported->Name == "_longjmpex")
    Binding.Routine = JumpRuntimeBinding::Kind::Resume;
  else if (Imported->Name == "_seh_longjmp_unwind")
    Binding.Routine = JumpRuntimeBinding::Kind::EH3Unwind;
  else
    return std::nullopt;
  Binding.Target = Target;
  Binding.ImportSlot = Slot;
  Binding.Indirect = Indirect;
  return Binding;
}

std::optional<JumpRuntimeBinding>
checkedJumpRuntimeCall(const BinaryImage &Image, const LowFunc &Function,
                       const LowOp &Op, size_t &Work) {
  if (Op.Opcode != NdOp::CALL || Op.NumInputs != 1 || Op.Seq < 0 ||
      !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4 ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !registration_abi::chargeCalleeWork(Work, 16))
    return std::nullopt;
  const auto Binding = checkedJumpRuntime(Image, Op.Inputs[0].Offset, false);
  if (!Binding || Op.Addr > UINT32_MAX - 5)
    return std::nullopt;
  for (const auto &Block : Function.Blocks) {
    if (!registration_abi::chargeCalleeWork(Work, Block.Ops.size() + 1))
      return std::nullopt;
    for (size_t I = 0; I != Block.Ops.size(); ++I) {
      if (&Block.Ops[I] != &Op)
        continue;
      if (auto Error = validateLowInstructionBoundaries(
              Block, LowInstructionBoundaryRequirement::Required)) {
        llvm::consumeError(std::move(Error));
        return std::nullopt;
      }
      for (const auto &Insn : Block.InstructionBoundaries) {
        if (!registration_abi::chargeCalleeWork(Work, 1))
          return std::nullopt;
        if (Insn.Address != Op.Addr || I < Insn.FirstOp ||
            I - Insn.FirstOp >= Insn.OpCount)
          continue;
        if (Insn.Size != 5 || Insn.Control != LowInstructionControl::Call ||
            Insn.Immediate != Binding->Target ||
            Insn.Mode != InstructionMode::Default ||
            Insn.TargetMode != LowInstructionTargetMode::Preserve ||
            hasLowInstructionControlFlag(
                Insn.ControlFlags, LowInstructionControlFlag::Conditional) ||
            hasLowInstructionControlFlag(
                Insn.ControlFlags, LowInstructionControlFlag::InstructionGuard))
          return std::nullopt;
        const auto Bytes = readImmutableCodeBytes(Image, Op.Addr, 5);
        if (!Bytes || (*Bytes)[0] != 0xe8 ||
            uint32_t(Op.Addr + 5 +
                     int64_t(readLE<int32_t>(Bytes->data() + 1))) !=
                Binding->Target)
          return std::nullopt;
        return Binding;
      }
      return std::nullopt;
    }
  }
  return std::nullopt;
}

bool isEH3JumpRuntimeInstruction(const BinaryImage &Image, va_t Address,
                                 uint16_t Size, llvm::ArrayRef<LowOp> Ops) {
  if (Size != 5 || Address > UINT32_MAX - Size)
    return false;
  const LowOp *Call = nullptr;
  for (const auto &Op : Ops)
    if (Op.Opcode == NdOp::CALL) {
      if (Call || Op.Addr != Address || Op.NumInputs != 1 ||
          !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4)
        return false;
      Call = &Op;
    }
  if (!Call)
    return false;
  const auto Binding = checkedJumpRuntime(Image, Call->Inputs[0].Offset, false);
  const auto Bytes = readImmutableCodeBytes(Image, Address, Size);
  return Binding &&
         (Binding->Routine == JumpRuntimeBinding::Kind::Capture ||
          Binding->Routine == JumpRuntimeBinding::Kind::Resume) &&
         Bytes && (*Bytes)[0] == 0xe8 &&
         uint32_t(Address + Size +
                  int64_t(readLE<int32_t>(Bytes->data() + 1))) ==
             Binding->Target;
}

} // namespace neverd::registration_state
