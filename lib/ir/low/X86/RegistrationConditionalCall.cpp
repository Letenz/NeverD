//===- RegistrationConditionalCall.cpp - Bound SEH call effects -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check invocation-specific SEH frame inputs and bounded effects.

#include "RegistrationABIPrivate.h"

#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/lift/X86Regs.h"

namespace neverd::registration_abi {
using namespace registration_state;

bool hasConditionalCallFrame(const BinaryImage &Image, const LowOp &Op,
                             const FrameState &State, size_t &Work) {
  if (Op.Opcode != NdOp::CALL || Op.NumInputs != 1 || !Op.Inputs[0].isConst() ||
      Op.Inputs[0].Size != 4)
    return false;
  const va_t Target = Op.Inputs[0].Offset;
  const auto SP =
      State.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset;
  if (!SP || *SP > INT32_MAX - 128 || Op.Opcode != NdOp::CALL)
    return false;
  std::map<int32_t, FrameValue> Arguments;
  for (int32_t Offset = 8; Offset <= 128; Offset += 4) {
    const auto It = State.Cells.find(*SP + Offset - 8);
    if (It != State.Cells.end() && !It->second.MayBeFrame)
      Arguments.emplace(Offset, It->second);
  }
  if (Arguments.empty())
    return false;
  Decoder Dec;
  if (!Dec.init(Image))
    return false;
  CFGBuilder Builder;
  auto Callee = Builder.build(Image, Dec, Target, "bound-seh-call");
  for (const auto &Block : Callee.Blocks)
    for (const auto &Insn : Block.InstructionBoundaries)
      if (!chargeCalleeWork(Work, 1) ||
          !hasImmutableCalleeInstruction(Image, Insn, Dec))
        return false;
  registration_abi::ImageFrameEffects Effects;
  if (Callee.ExceptionMetadata || !Callee.hasCompleteLiftCoverage() ||
      !registration_abi::chargeDecodedCallee(Work, Callee) ||
      !registration_abi::hasPrivateCallerFrame(
          Callee, Image, Work, Effects, false, nullptr, nullptr, &Arguments) ||
      !Effects.Writes.empty() || !Effects.CallerPCWrites.empty())
    return false;
  return true;
}

} // namespace neverd::registration_abi
