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
    BlockFacts &Facts, std::optional<FrameValue> &CallSP,
    std::optional<Domain> &CallException) {
  const auto &Block = Function.Blocks[I];
  if (!CallSP && BoundCallees && !KnownCxx && !After.Unknown &&
      !Facts.Invalid && After.Parent && !After.Callback &&
      After.JumpBindingsIntact && !After.Frame.CallbackEntry &&
      After.Installed && !After.Uninstalled &&
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
      if (auto Effect =
              (*BoundCallees)(Op, After.Frame, WorkUsed, After.JumpCaptures)) {
        const auto SP = parentStackOffset(After);
        if (SP && int64_t(*SP) + Effect->StackPopBytes <= INT32_MAX) {
          Result.HasBoundSEHCallees = true;
          if (Effect->NativeCall) {
            const auto [It, New] = BoundCalls.emplace(
                std::make_pair(Op.Addr, Op.Seq), *Effect->NativeCall);
            if (!New && It->second != *Effect->NativeCall)
              Result.SEHBoundCallsComplete = false;
          } else
            Result.SEHBoundCallsComplete = false;
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
            invalidateJumpCaptures(After, FrameValue::frame(Write.Begin),
                                   Width);
            After.Frame.store(Write.Begin, Width, {});
            // A fault can dispatch the caller after a partial helper effect.
            // Do not lend pre-call object contents to its filters.
            CallException->Frame.store(Write.Begin, Width, {});
            invalidateJumpCaptures(*CallException,
                                   FrameValue::frame(Write.Begin), Width);
          }
          for (const auto &Resume : Effect->JumpResumes) {
            const auto Capture =
                llvm::find_if(After.JumpCaptures, [&](const auto &Item) {
                  return Item.second.Call == Resume.CaptureCall;
                });
            if (!After.JumpBindingsIntact ||
                Capture == After.JumpCaptures.end() ||
                !hasEH3JumpCapture(After.Frame, Capture->second)) {
              Facts.Invalid = After.Unknown = true;
              After.Levels.clear();
              Result.SEHJumpResumptionsComplete = false;
              break;
            }
            BlockFacts ResumeFacts = Facts;
            resumeJumpCapture(After, ResumeFacts, Op, Capture->second,
                              Resume.Value, Op.Inputs[0].Offset,
                              Resume.Address);
            Facts.Invalid |= ResumeFacts.Invalid;
          }
          if (!Effect->HasOrdinaryReturn && !Effect->JumpResumes.empty())
            Facts.InvocationAtExit = true;
        }
      }
  return true;
}

} // namespace neverd::registration_state
