//===- RegistrationJumpCapture.cpp - Source-bound EH3 jump capture --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check the physical capture call, established frame and bounded buffer.

#include "RegistrationJumpCapture.h"

#include "RegistrationABIPrivate.h"
#include "RegistrationJumpRuntime.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/LowNoReturn.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

namespace neverd::registration_state {

std::optional<EH3JumpCapture>
prepareEH3JumpCapture(const BinaryImage &Image, const LowFunc &Function,
                      const LowOp &Op, const FrameState &Frame, bool Installed,
                      size_t &Work) {
  if (!Installed || !Function.ExceptionMetadata ||
      !Function.ExceptionMetadata->Registration ||
      Function.ExceptionMetadata->Cxx || Frame.CallbackEntry ||
      Op.Opcode != NdOp::CALL || Op.NumInputs != 1 || Op.Seq < 0 ||
      !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4 ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !registration_abi::chargeCalleeWork(Work, 16))
    return std::nullopt;
  const auto &EH = *Function.ExceptionMetadata;
  const auto &Chain = *EH.Registration;
  if (Function.Entry != EH.CodeRange.Begin || !EH.CodeRange.contains(Op.Addr) ||
      Op.Addr > UINT32_MAX - 5 || Op.Addr + 5 > EH.CodeRange.End ||
      EH.ParseStatus != ExceptionParseStatus::Complete ||
      EH.Encoding != ExceptionEncoding::X86ScopeTableEH3 ||
      EH.Personality != ExceptionPersonality::ExceptHandler3 ||
      Chain.RealignedFrame || Chain.RegistrationOffset != -16 ||
      Chain.TryLevelOffset != -4 || Chain.SeededTryLevel != -1 ||
      Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride].Offset != 0)
    return std::nullopt;
  const auto Runtime = checkedJumpRuntime(Image, Op.Inputs[0].Offset, false);
  if (!Runtime || Runtime->Routine != JumpRuntimeBinding::Kind::Capture)
    return std::nullopt;
  bool SourceFound = false;
  for (const auto &Block : Function.Blocks)
    for (size_t I = 0; I != Block.Ops.size(); ++I) {
      if (!registration_abi::chargeCalleeWork(Work, 1))
        return std::nullopt;
      if (&Block.Ops[I] != &Op)
        continue;
      if (auto Error = validateLowInstructionBoundaries(
              Block, LowInstructionBoundaryRequirement::Required)) {
        llvm::consumeError(std::move(Error));
        return std::nullopt;
      }
      for (const auto &Insn : Block.InstructionBoundaries)
        if (!registration_abi::chargeCalleeWork(Work, 1))
          return std::nullopt;
        else if (Insn.Address == Op.Addr && I >= Insn.FirstOp &&
                 I - Insn.FirstOp < Insn.OpCount) {
          if (Insn.Size != 5 || Insn.Control != LowInstructionControl::Call ||
              Insn.Immediate != Op.Inputs[0].Offset ||
              Insn.Mode != InstructionMode::Default ||
              Insn.TargetMode != LowInstructionTargetMode::Preserve ||
              isUnconditionalNoReturn(Insn) ||
              hasLowInstructionControlFlag(
                  Insn.ControlFlags, LowInstructionControlFlag::Conditional) ||
              hasLowInstructionControlFlag(
                  Insn.ControlFlags,
                  LowInstructionControlFlag::InstructionGuard))
            return std::nullopt;
          SourceFound = true;
        }
    }
  if (!SourceFound || Op.Addr > UINT32_MAX - 5)
    return std::nullopt;
  const auto Bytes = readImmutableCodeBytes(Image, Op.Addr, 5);
  if (!Bytes || (*Bytes)[0] != 0xe8 ||
      uint32_t(Op.Addr + 5 + int64_t(readLE<int32_t>(Bytes->data() + 1))) !=
          Runtime->Target)
    return std::nullopt;
  const auto SP =
      Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset;
  if (!SP || *SP < INT32_MIN + 4 || *SP > INT32_MAX - 16)
    return std::nullopt;
  const auto Buffer = Frame.load(*SP, 4);
  const auto Level = Frame.load(*SP + 12, 4).Constant;
  const auto Unwind = Frame.load(*SP + 8, 4).Constant;
  if (!Buffer.Offset || Buffer.ReturnPC || Buffer.CallbackAddress ||
      Buffer.EntryOffset || Buffer.SEHException ||
      Frame.load(*SP + 4, 4).Constant != 2 || !Level || !Unwind ||
      Frame.load(-4, 4).Constant != Level ||
      (!Frame.load(-16, 4).PreviousChain) ||
      Frame.load(-12, 4).Constant != EH.PersonalityVA ||
      Frame.load(-8, 4).Constant != Chain.ScopeTableVA)
    return std::nullopt;
  const int64_t End = int64_t(*Buffer.Offset) + EH3JumpCapture::BufferBytes;
  if (*Buffer.Offset < int64_t(*SP) + 16 || End > -24 ||
      (*Level != UINT32_MAX && *Level >= Chain.Scopes.size()))
    return std::nullopt;
  const auto UnwindRuntime = checkedJumpRuntime(Image, *Unwind, false);
  if (!UnwindRuntime ||
      UnwindRuntime->Routine != JumpRuntimeBinding::Kind::EH3Unwind)
    return std::nullopt;
  EH3JumpCapture Capture;
  Capture.Call = Op.Addr;
  Capture.ReturnPC = Op.Addr + 5;
  Capture.UnwindTarget = *Unwind;
  Capture.BufferOffset = *Buffer.Offset;
  Capture.StackOffset = *SP;
  Capture.Level = int32_t(*Level);
  constexpr uint64_t Registers[] = {x86reg::RBP, x86reg::RBX, x86reg::RDI,
                                    x86reg::RSI};
  for (unsigned I = 0; I != std::size(Registers); ++I)
    Capture.Nonvolatile[I] =
        Frame.Registers[Registers[I] / x86reg::GeneralRegStride];
  return Capture;
}

} // namespace neverd::registration_state
