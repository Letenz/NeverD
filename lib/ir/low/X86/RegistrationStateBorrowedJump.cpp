//===- RegistrationStateBorrowedJump.cpp - Bound caller buffer exits ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Carries an opaque caller capture identity through a checked helper.

#include "RegistrationStateSolver.h"

#include "neverd/loader/RegistrationScopeGraph.h"

namespace neverd::registration_state {

bool RegistrationStateSolver::transferBorrowedJump(size_t I, Domain &After,
                                                   const LowOp &Op,
                                                   BlockFacts &Facts,
                                                   const FrameValue &Buffer,
                                                   const FrameValue &Value) {
  const auto Refuse = [&] {
    Facts.Invalid = After.Unknown = true;
    After.Levels.clear();
    Result.SEHJumpResumptionsComplete = false;
    if (Conditional)
      Conditional->Complete = false;
    return true;
  };
  // The enclosing caller owns the buffer bytes, capture and remaining chain.
  // This callee proves only its retired local chain and concrete borrowed
  // object effects. It cannot read the capture as source frame memory.
  if (!Conditional || !Conditional->Complete || !Buffer.JumpCapture ||
      !Conditional->JumpBuffers.count(*Buffer.JumpCapture) || Buffer.Offset ||
      Buffer.EntryOffset || Buffer.CallbackAddress || Buffer.SEHException ||
      Buffer.ReturnPC || !Value.isFrameIndependentScalar() ||
      After.Levels != std::set<int32_t>{registrationOutsideTryLevel(EH4)} ||
      (EH4 && (!CompleteCookies || !cookiesReady(After.Frame))) ||
      (After.Callback && !validateSEHInvocationBlock(I)) ||
      !charge(Conditional->JumpResumes.size() + 1))
    return Refuse();
  BorrowedJumpResume Resume{Op.Addr, Op.Seq, *Buffer.JumpCapture, Value};
  const auto [It, New] =
      Conditional->JumpResumes.emplace(std::make_pair(Op.Addr, Op.Seq), Resume);
  if (!New && It->second != Resume)
    return Refuse();
  Facts.InvocationAtExit = true;
  return true;
}

} // namespace neverd::registration_state
