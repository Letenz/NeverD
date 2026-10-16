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
  if (Value.MayBeFrame)
    return false;
  return mergeSEHFrameOutcome(Jump.Resumed[Value], State);
}

} // namespace neverd::registration_state
