//===- RegistrationStateRuntimeCall.cpp - Native SEH runtime arguments ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Publish native projections only after the runtime's source effect proof.

#include "RegistrationStateSolver.h"

#include "neverd/lift/X86Regs.h"

namespace neverd::registration_state {

void RegistrationStateSolver::recordSEHRuntimeCall(
    const Domain &State, const LowOp &Op, RegistrationSEHBoundCall::Kind Kind,
    std::optional<int32_t> Output) {
  if (Conditional || KnownCxx || EH4 || !State.Parent || State.Callback ||
      State.Unknown || !State.Installed || State.Uninstalled)
    return;
  const auto SP =
      State.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset;
  if (!SP || !charge(5))
    return;
  RegistrationSEHBoundCall Call;
  Call.CalleeKind = Kind;
  Call.Address = Op.Addr;
  Call.OpSeq = Op.Seq;
  Call.TargetVA = Op.Inputs[0].Offset;
  Call.StackOffset = *SP;
  using Argument = RegistrationSEHBoundArgument::Kind;
  const unsigned Count =
      Kind == RegistrationSEHBoundCall::Kind::ControlFP ? 3 : 4;
  for (unsigned I = 0; I != Count; ++I)
    Call.Arguments.push_back({Argument::Scalar, *SP + int32_t(I * 4), 0});
  if (Kind == RegistrationSEHBoundCall::Kind::RaiseException)
    Call.Arguments.back() = {Argument::NullPointer, 0, 0};
  else if (Kind == RegistrationSEHBoundCall::Kind::ControlFP && Output)
    Call.Arguments.front() = {Argument::Object, *Output, 0};
  else
    return;
  const auto [It, Added] =
      BoundCalls.emplace(std::make_pair(Op.Addr, Op.Seq), Call);
  if (!Added && It->second != Call)
    Result.SEHBoundCallsComplete = false;
  Result.HasBoundSEHCallees = true;
}

} // namespace neverd::registration_state
