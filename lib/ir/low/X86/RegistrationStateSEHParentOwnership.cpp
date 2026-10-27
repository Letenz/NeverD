//===- RegistrationStateSEHParentOwnership.cpp - Parent continuations ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Anchors callback-only continuation tails in already owned parent code.

#include "RegistrationStateSolver.h"

namespace neverd::registration_state {

bool RegistrationStateSolver::extendSEHParentOwnership() {
  const size_t Count = Function.Blocks.size();
  if (!charge(Count + Chain.Scopes.size() + 1))
    return false;
  std::set<va_t> CallbackRoots;
  for (const auto &Scope : Chain.Scopes)
    CallbackRoots.insert(Scope.IsFinally ? Scope.HandlerVA : Scope.FilterVA);
  std::vector<bool> Owned(Count), Eligible(Count);
  std::vector<size_t> Remaining(Count);
  std::vector<std::vector<size_t>> Predecessors(Count);
  std::vector<size_t> Pending;
  for (size_t I = 0; I != Count; ++I)
    Owned[I] = SEHParentEntries.count(Function.Blocks[I].StartAddr) != 0;
  for (size_t I = 0; I != Count; ++I) {
    const auto &Block = Function.Blocks[I];
    if (Owned[I] || CallbackRoots.count(Block.StartAddr) ||
        Block.Succs.empty() || !EH.ownsCode({Block.StartAddr, Block.EndAddr}))
      continue;
    if (!validateSEHInvocationBlock(I)) {
      if (Exhausted)
        return false;
      continue;
    }
    const auto &Terminator = Block.InstructionBoundaries.back();
    if (hasLowInstructionControlFlag(
            Terminator.ControlFlags,
            LowInstructionControlFlag::InstructionGuard))
      continue;
    if (Terminator.Control == LowInstructionControl::Branch &&
        (Terminator.Immediate != Block.Ops.back().Inputs[0].Offset ||
         hasLowInstructionControlFlag(Terminator.ControlFlags,
                                      LowInstructionControlFlag::Conditional) !=
             (Block.Ops.back().Opcode == NdOp::COND_BR)))
      continue;
    Eligible[I] = true;
    for (int Next : Block.Succs) {
      if (!charge(1))
        return false;
      const size_t Target = Index.at(Next);
      if (!Owned[Target]) {
        ++Remaining[I];
        Predecessors[Target].push_back(I);
      }
    }
    if (!Remaining[I])
      Pending.push_back(I);
  }
  // Every successor must have an existing parent anchor. A cycle or an
  // unowned terminal return cannot manufacture one. This grants no reaching
  // state: the exact SavedESP transfer still proves the parent invocation.
  while (!Pending.empty()) {
    const size_t I = Pending.back();
    Pending.pop_back();
    if (!charge(1) || !Eligible[I] || Owned[I] || Remaining[I])
      return false;
    Owned[I] = true;
    SEHParentContinuations.insert(Function.Blocks[I].StartAddr);
    for (size_t Previous : Predecessors[I]) {
      if (!charge(1) || !Remaining[Previous])
        return false;
      if (!--Remaining[Previous])
        Pending.push_back(Previous);
    }
  }
  return true;
}

} // namespace neverd::registration_state
