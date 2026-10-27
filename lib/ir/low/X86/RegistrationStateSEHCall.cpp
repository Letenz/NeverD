//===- RegistrationStateSEHCall.cpp - Physical finally call continuation --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate ordinary finally continuations for direct and runtime replay.

#include "RegistrationControl.h"
#include "RegistrationStateSolver.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/low/LowNoReturn.h"

namespace neverd::registration_state {

std::optional<va_t>
RegistrationStateSolver::sehCallContinuation(va_t Call, va_t CallerEntry) {
  if (!charge(1) || (CallerEntry && !FinallyTargets.count(CallerEntry)))
    return std::nullopt;
  const auto Source = SEHControlIndex.find(Call);
  if (Source == SEHControlIndex.end() ||
      Source->second.Control != RegistrationSEHControlContract::Kind::NearCall)
    return std::nullopt;
  // A nested invocation resumes on its authenticated caller's private stack.
  // Out-of-line finally bodies need not lie inside the primary code span.
  va_t Next = Source->second.EndAddress;
  std::set<va_t> Seen;
  while (!Entries.count(Next)) {
    if (!charge(5) ||
        Seen.size() ==
            ::neverd::limits::ir::
                kRegistrationStateSEHCallRegistrationStateSolverSehCallContinuationSeenSizeLimit ||
        !Seen.insert(Next).second)
      return std::nullopt;
    const auto Jump = SEHControlIndex.find(Next);
    if (Jump == SEHControlIndex.end() ||
        Jump->second.Control !=
            RegistrationSEHControlContract::Kind::NearJump ||
        (Jump->second.EndAddress - Next != 2 &&
         Jump->second.EndAddress - Next != 5) ||
        (!CallerEntry && !EH.CodeRange.contains(Next)))
      return std::nullopt;
    Next = Jump->second.Target;
  }
  return CallerEntry || EH.CodeRange.contains(Next) ? std::optional<va_t>{Next}
                                                    : std::nullopt;
}

bool RegistrationStateSolver::validateSEHCallContinuation(size_t I,
                                                          const LowOp &Op,
                                                          va_t CallerEntry) {
  const auto &Block = Function.Blocks[I];
  const auto Boundary = Boundaries.find(Op.Addr);
  const auto Control = SEHControlIndex.find(Op.Addr);
  const auto Resume = sehCallContinuation(Op.Addr, CallerEntry);
  if (!SEHControls || !Function.hasCompleteLiftCoverage() ||
      Op.Opcode != NdOp::CALL || Op.NumInputs != 1 || !Op.Inputs[0].isConst() ||
      Op.Inputs[0].Size != 4 || Op.Seq < 0 ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Boundary == Boundaries.end() || Boundary->second.first != Block.Id ||
      Control == SEHControlIndex.end() ||
      Control->second.Control !=
          RegistrationSEHControlContract::Kind::NearCall ||
      Control->second.Target != Op.Inputs[0].Offset ||
      Control->second.EndAddress > Block.EndAddr || Block.Succs.size() != 1 ||
      !Index.count(Block.Succs.front()) || !Resume ||
      Function.Blocks[Index.at(Block.Succs.front())].StartAddr != *Resume ||
      !charge(Block.Ops.size() + Block.InstructionBoundaries.size() + 1))
    return false;
  if (auto Error = validateLowInstructionBoundaries(
          Block, LowInstructionBoundaryRequirement::Required)) {
    llvm::consumeError(std::move(Error));
    return false;
  }
  const auto &Insn = Boundary->second.second;
  const size_t OpIndex = &Op - Block.Ops.data();
  if (Insn.Control != LowInstructionControl::Call ||
      Insn.Size != Control->second.EndAddress - Op.Addr ||
      Insn.Immediate != Control->second.Target ||
      Insn.Mode != InstructionMode::Default ||
      Insn.TargetMode != LowInstructionTargetMode::Preserve ||
      hasLowInstructionControlFlag(Insn.ControlFlags,
                                   LowInstructionControlFlag::Conditional) ||
      hasLowInstructionControlFlag(
          Insn.ControlFlags, LowInstructionControlFlag::InstructionGuard) ||
      isUnconditionalNoReturn(Insn) || Insn.FirstOp > OpIndex ||
      OpIndex - Insn.FirstOp + 1 != Insn.OpCount)
    return false;
  // Entering the callback suspends this block at the source CALL. There is no
  // transfer step for another operation within that same physical instruction.
  if (Control->second.EndAddress == Block.EndAddr)
    return true;

  // A CALL followed by a direct jump can share one LowIR block. The physical
  // return PC remains the CALL's end. Only that authenticated branch may
  // follow it; additional register or memory effects need ordinary transfer.
  const auto &Tail = Block.InstructionBoundaries.back();
  const auto Jump = SEHControlIndex.find(Control->second.EndAddress);
  if (Tail.Address != Control->second.EndAddress ||
      Tail.Address + Tail.Size != Block.EndAddr ||
      Tail.Control != LowInstructionControl::Branch ||
      Tail.ControlFlags != LowInstructionControlFlag::Branch ||
      Tail.Mode != InstructionMode::Default ||
      Tail.TargetMode != LowInstructionTargetMode::Preserve ||
      Tail.FirstOp != Insn.FirstOp + Insn.OpCount || Tail.OpCount != 1 ||
      Jump == SEHControlIndex.end() ||
      Jump->second.Control != RegistrationSEHControlContract::Kind::NearJump ||
      Jump->second.EndAddress != Block.EndAddr ||
      Tail.Immediate != Jump->second.Target)
    return false;
  const auto &Branch = Block.Ops[Tail.FirstOp];
  return Branch.Opcode == NdOp::BRANCH && Branch.NumInputs == 1 &&
         !Branch.Output.Size && Branch.Seq >= 0 &&
         Branch.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
         Branch.MemoryOrdering == NdMemoryOrdering::None &&
         isPE32CodeTarget(Branch.Inputs[0]) &&
         Branch.Inputs[0].Offset == Jump->second.Target;
}

} // namespace neverd::registration_state
