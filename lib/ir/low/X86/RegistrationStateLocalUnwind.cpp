//===- RegistrationStateLocalUnwind.cpp - checked SEH local unwinds ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/lift/X86Regs.h"

namespace neverd::registration_state {

std::optional<FrameValue> RegistrationStateSolver::transferLocalUnwind(
    size_t I, Domain &After, const LowOp &Op, BlockFacts &Facts) {
  if ((Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL) ||
      Op.NumInputs != 1 || !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4 ||
      !LocalUnwindTargets.count(
          {Op.Inputs[0].Offset, Op.Opcode == NdOp::INDIR_CALL}))
    return std::nullopt;
  auto Refuse = [&]() -> std::optional<FrameValue> {
    FailedLocalUnwind = Facts.Invalid = After.Unknown = true;
    After.Levels.clear();
    return std::nullopt;
  };
  const auto &Block = Function.Blocks[I];
  const auto Boundary = Boundaries.find(Op.Addr);
  const auto SP = After.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride];
  if (!Function.hasCompleteLiftCoverage() || KnownCxx || EH4 ||
      EH.Encoding != ExceptionEncoding::X86ScopeTableEH3 ||
      Chain.RealignedFrame || Chain.RegistrationOffset != -16 ||
      Chain.TryLevelOffset != -4 || Chain.SeededTryLevel != -1 ||
      !After.Installed || After.Uninstalled || After.Unknown || Facts.Invalid ||
      !SP.Offset || *SP.Offset > INT32_MAX - 4 || Op.Seq < 0 ||
      Boundary == Boundaries.end() || Boundary->second.first != Block.Id ||
      Boundary->second.second.Control != LowInstructionControl::Call ||
      hasLowInstructionControlFlag(Boundary->second.second.ControlFlags,
                                   LowInstructionControlFlag::Conditional) ||
      Boundary->second.second.Address + Boundary->second.second.Size !=
          Block.EndAddr ||
      After.Frame.load(*SP.Offset, 4).Offset != Chain.RegistrationOffset)
    return Refuse();
  const auto &Insn = Boundary->second.second;
  const size_t OpIndex = &Op - Block.Ops.data();
  if (Insn.FirstOp > OpIndex || OpIndex - Insn.FirstOp >= Insn.OpCount ||
      Insn.FirstOp > Block.Ops.size() ||
      Insn.OpCount > Block.Ops.size() - Insn.FirstOp)
    return Refuse();
  const auto Bound = After.Frame.load(*SP.Offset + 4, 4).Constant;
  // Only the current level indexes the scope table. The requested bound may
  // be any constant uint32_t; rejecting an out-of-table bound changes no-op
  // unwinds that the runtime accepts. Dynamic bounds remain unproved.
  if (!Bound || After.Levels.empty())
    return Refuse();
  std::optional<FrameState> Joined;
  std::set<int32_t> StoppedLevels;
  for (int32_t From : After.Levels) {
    if (!charge(After.Frame.cellCount() + 1))
      return Refuse();
    FrameState Frame = After.Frame;
    int32_t Walk = From;
    size_t Steps = 0;
    // EH3's runtime stops at -1, or at any unsigned level not above the
    // requested bound. An enclosing link can skip that bound entirely, and
    // an already retired registration must not execute another finally.
    while (Walk != -1 && (*Bound == UINT32_MAX || uint32_t(Walk) > *Bound)) {
      if (!charge(1) || Walk < 0 || size_t(Walk) >= Chain.Scopes.size() ||
          ++Steps > Chain.Scopes.size())
        return Refuse();
      const auto &Scope = Chain.Scopes[Walk];
      Walk = Scope.EnclosingLevel;
      Frame.store(*Chain.TryLevelOffset, 4,
                  FrameValue::constant(uint32_t(Walk)));
      if (Scope.IsFinally) {
        std::vector<RegistrationObjectExtent> Writes;
        if (!runLocalFinally(Scope.HandlerVA, Frame, false, &Writes))
          return Refuse();
        for (const auto &Write : Writes)
          invalidateJumpCaptures(After, FrameValue::frame(Write.Begin),
                                 uint16_t(Write.End - Write.Begin));
      }
    }
    StoppedLevels.insert(Walk);
    if (!Joined)
      Joined = std::move(Frame);
    else if (!charge(Joined->cellCount() + Frame.cellCount() + 10))
      return Refuse();
    else
      Joined->merge(Frame);
  }
  After.Frame.Cells = std::move(Joined->Cells);
  After.Levels = std::move(StoppedLevels);
  if (After.Levels.size() == 1)
    After.Frame.store(*Chain.TryLevelOffset, 4,
                      FrameValue::constant(uint32_t(*After.Levels.begin())));
  // _local_unwind2 is caller-cleanup. Its proved returning path retains the
  // argument-area ESP, independently of every memory/native-output contract.
  return SP;
}

} // namespace neverd::registration_state
