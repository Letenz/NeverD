//===- RegistrationStateFinallyCall.cpp - Checked ordinary finally calls --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/ir/low/LowNoReturn.h"
#include "neverd/lift/X86Regs.h"

namespace neverd::registration_state {

bool RegistrationStateSolver::transferOrdinaryFinally(
    size_t I, Domain &After, const LowOp &Op, BlockFacts &Facts,
    std::optional<FrameValue> &ReturnedSP) {
  if (KnownCxx || Op.Opcode != NdOp::CALL || Op.NumInputs != 1 ||
      !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4)
    return false;
  if (!FinallyTargets.count(Op.Inputs[0].Offset))
    return false;
  // An existing authenticated callable contract retains its own transfer and
  // native obligations. The frame-sharing interpreter fills only its absence.
  if (CheckCalls && CalleeIndices.count(Op.Inputs[0].Offset))
    return false;
  // This inspection grants state flow, not an independently callable ABI or
  // permission to emit a native call to a source-frame-sharing helper.
  if (CheckCalls)
    CompleteCalls = false;
  if (SEHControls && !StackPops.count({Op.Inputs[0].Offset, false}))
    return enterOrdinaryFinally(I, After, Op, Facts);
  const auto Refuse = [&] {
    Facts.Invalid = After.Unknown = true;
    After.Levels.clear();
    After.InitializedFrameBytes.clear();
    if (charge(After.Frame.cellCount()))
      After.Frame.forgetCellValues();
    return true;
  };
  const auto Boundary = Boundaries.find(Op.Addr);
  const auto &Block = Function.Blocks[I];
  const auto SP = After.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride];
  const auto SavedSP =
      After.Frame.load(*Chain.RegistrationOffset - 8, 4).Offset;
  if (!Function.hasCompleteLiftCoverage() || After.Unknown || Facts.Invalid ||
      !After.Parent || After.Callback || After.Frame.CallbackEntry ||
      Chain.RealignedFrame || !SP.Offset || !SavedSP || *SP.Offset > *SavedSP ||
      *SP.Offset < INT32_MIN + 4 ||
      After.Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride].Offset !=
          0 ||
      Op.Seq < 0 || Boundary == Boundaries.end() ||
      Boundary->second.first != Block.Id)
    return Refuse();
  const auto &Insn = Boundary->second.second;
  const auto OpIndex = size_t(&Op - Block.Ops.data());
  // The independent byte checker admits near RET encodings. LowIR's generic
  // RETURN alone also represents far/interrupt returns on x86, so it cannot
  // supply that architectural fact. The pop count still proves neither the
  // actual return PC nor any frame-memory effect inspected below.
  const auto Stack = StackPops.find({Op.Inputs[0].Offset, false});
  if (Stack == StackPops.end() || Stack->second != 0)
    return Refuse();
  if (!charge(Block.Ops.size() + Block.InstructionBoundaries.size() + 1))
    return Refuse();
  if (auto Error = validateLowInstructionBoundaries(
          Block, LowInstructionBoundaryRequirement::Required)) {
    llvm::consumeError(std::move(Error));
    return Refuse();
  }
  if (Insn.Control != LowInstructionControl::Call ||
      isUnconditionalNoReturn(Insn) ||
      hasLowInstructionControlFlag(Insn.ControlFlags,
                                   LowInstructionControlFlag::Conditional) ||
      hasLowInstructionControlFlag(
          Insn.ControlFlags, LowInstructionControlFlag::InstructionGuard) ||
      Insn.FirstOp > OpIndex || OpIndex - Insn.FirstOp >= Insn.OpCount ||
      Insn.FirstOp > Block.Ops.size() ||
      Insn.OpCount > Block.Ops.size() - Insn.FirstOp ||
      !runLocalFinally(Op.Inputs[0].Offset, After.Frame, true))
    return Refuse();
  ReturnedSP = SP;
  return true;
}

} // namespace neverd::registration_state
