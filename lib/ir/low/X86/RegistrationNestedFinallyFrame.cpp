//===- RegistrationNestedFinallyFrame.cpp - Suspended finally calls ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Replays a returning finally inside an already active unwind callback.

#include "RegistrationStateSolver.h"

#include "neverd/Limits.h"
#include "neverd/lift/X86Regs.h"

namespace neverd::registration_state {

bool RegistrationStateSolver::runSEHNestedFinallyFrame(
    size_t I, const LowOp &Op, FrameState &State, int32_t ParentStackOffset,
    std::vector<RegistrationObjectExtent> *Writes, unsigned Depth) {
  const auto SP =
      State.Registers[x86reg::RSP / x86reg::GeneralRegStride].CallbackAddress;
  if (Depth >= 16 || !State.CallbackEntry || State.SEHFilterEntry || !SP ||
      SP->Entry != State.CallbackEntry || SP->Offset > 0 ||
      SP->Offset < -int64_t(limits::kMaxSourceCallBorrowedBytes) ||
      State.Registers[x86reg::RBP / x86reg::GeneralRegStride].Offset != 0 ||
      !validateSEHInvocationBlock(I) || !validateSEHCallContinuation(I, Op) ||
      !charge(State.cellCount() + State.OtherRegisterBytes.size() + 10))
    return false;

  FrameState Returned = State;
  // This call must return through its own checked near RET. A nested nonlocal
  // transfer cannot be substituted for an ordinary continuation. Each nested
  // invocation shares the global work budget and has a bounded call depth.
  if (!runSEHCallbackFrame(Op.Inputs[0].Offset, Returned, true, Writes, nullptr,
                           nullptr, nullptr, Depth + 1, ParentStackOffset))
    return false;
  Returned.leaveCallback();
  Returned.CallbackEntry = State.CallbackEntry;
  Returned.CallbackCells = std::move(State.CallbackCells);
  Returned.InitializedCallbackBytes = std::move(State.InitializedCallbackBytes);
  Returned.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
      FrameValue::callbackFrame(SP->Entry, SP->Offset);
  State = std::move(Returned);
  return true;
}

} // namespace neverd::registration_state
