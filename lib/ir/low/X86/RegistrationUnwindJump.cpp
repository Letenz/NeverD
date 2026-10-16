//===- RegistrationUnwindJump.cpp - Repeated nonlocal callback jumps -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Separates nonlocal callback exits from ordinary returns and selections.

#include "RegistrationStateSolver.h"

#include "neverd/lift/X86Regs.h"

namespace neverd::registration_state {

bool RegistrationStateSolver::recordSEHUnwindJump(size_t I, const LowOp &Op,
                                                  const FrameState &State,
                                                  va_t Callback,
                                                  SEHUnwindJump &Jump) {
  const auto SP =
      State.Registers[x86reg::RSP / x86reg::GeneralRegStride].CallbackAddress;
  if (!JumpRuntime || !JumpRuntime->Resume ||
      !JumpRuntime->Resume(Op, WorkUsed) || !validateSEHInvocationBlock(I) ||
      !SP || SP->Entry != Callback || SP->Offset > -8 ||
      !State.callbackMemoryIsPrivate(*SP, 8, true) ||
      State.loadCallback(SP->Offset, 4).Offset != Jump.Capture.BufferOffset ||
      !hasSEHJumpCapture(State, Jump.Capture))
    return false;
  const auto Value = State.loadCallback(SP->Offset + 4, 4);
  const auto Normalize = [](const FrameValue &V) {
    return V.Constant == 0 ? FrameValue::constant(1) : V;
  };
  // A different capture or value requires its own continuation recipe.
  if (!Value.Constant || !Jump.Value.Constant || Value.MayBeFrame ||
      Jump.Value.MayBeFrame || Normalize(Value) != Normalize(Jump.Value))
    return false;
  return mergeSEHFrameOutcome(Jump.Resumed, State);
}

} // namespace neverd::registration_state
