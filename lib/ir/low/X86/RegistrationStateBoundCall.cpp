//===- RegistrationStateBoundCall.cpp - Original SEH helper transfers -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind original helper effects to one checked caller occurrence.

#include "RegistrationStateSolver.h"

#include "neverd/lift/X86Regs.h"

namespace neverd::registration_state {

bool RegistrationStateSolver::transferBoundSEHCallee(
    size_t I, const Domain &Before, Domain &After, const LowOp &Op,
    const BlockFacts &Facts, std::optional<FrameValue> &CallSP,
    std::optional<Domain> &CallException) {
  const auto &Block = Function.Blocks[I];
  if (!CallSP && BoundCallees && !KnownCxx && !After.Unknown &&
      !Facts.Invalid && After.Parent && !After.Callback &&
      !After.Frame.CallbackEntry && After.Installed && !After.Uninstalled &&
      After.Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride].Offset ==
          0 &&
      Chain.RegistrationOffset == -16 && Chain.TryLevelOffset == -4 &&
      !Chain.RealignedFrame && Function.hasCompleteLiftCoverage() &&
      Op.Opcode == NdOp::CALL &&
      !FinallyTargets.count(Op.NumInputs && Op.Inputs[0].isConst()
                                ? Op.Inputs[0].Offset
                                : InvalidVA))
    if (const auto Source = Boundaries.find(Op.Addr);
        Op.Seq >= 0 && Source != Boundaries.end() &&
        Source->second.first == Block.Id &&
        Source->second.second.Control == LowInstructionControl::Call &&
        Op.NumInputs == 1 && Op.Inputs[0].isConst() && Op.Inputs[0].Size == 4 &&
        Source->second.second.Immediate == Op.Inputs[0].Offset &&
        size_t(&Op - Block.Ops.data()) >= Source->second.second.FirstOp &&
        size_t(&Op - Block.Ops.data()) - Source->second.second.FirstOp <
            Source->second.second.OpCount)
      if (auto Effect = (*BoundCallees)(Op, After.Frame, WorkUsed)) {
        const auto SP = parentStackOffset(After);
        if (SP && int64_t(*SP) + Effect->StackPopBytes <= INT32_MAX) {
          Result.HasBoundSEHCallees = true;
          CallSP =
              FrameValue::frame(int32_t(int64_t(*SP) + Effect->StackPopBytes));
          if (!Effect->Writes.empty()) {
            if (!charge(Before.Frame.cellCount() + 1))
              return false;
            if (!CallException)
              CallException = Before;
          }
          for (const auto &Write : Effect->Writes) {
            const auto Width = uint16_t(Write.End - Write.Begin);
            After.Frame.store(Write.Begin, Width, {});
            // A fault can dispatch the caller after a partial helper effect.
            // Do not lend pre-call object contents to its filters.
            CallException->Frame.store(Write.Begin, Width, {});
          }
        }
      }
  return true;
}

} // namespace neverd::registration_state
