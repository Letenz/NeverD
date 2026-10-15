//===- RegistrationConditionalFrame.h - Bound callee inputs -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONCONDITIONALFRAME_H
#define NEVERD_IR_LOW_X86_REGISTRATIONCONDITIONALFRAME_H
/// \file
/// Check invocation-specific SEH frame inputs and bounded effects.

#include "RegistrationFrame.h"
#include "RegistrationJumpCapture.h"

#include "neverd/ir/RegistrationCall.h"
#include "neverd/ir/RegistrationState.h"

#include <functional>
#include <map>
#include <set>

namespace neverd {
struct BinaryImage;
} // namespace neverd

namespace neverd::registration_state {

struct BorrowedJumpResume {
  va_t Address = InvalidVA;
  int Seq = -1;
  va_t CaptureCall = InvalidVA;
  FrameValue Value;
  bool operator==(const BorrowedJumpResume &) const = default;
};

/// Invocation-specific inputs in the callee's established EBP coordinates.
/// Positive object extents are proved caller storage, disjoint from this
/// invocation's return PC, private frame and incoming argument slots.
struct ConditionalFrame {
  std::function<bool(const LowOp &, const FrameState &)> ProveCall;
  std::map<int32_t, FrameValue> Arguments;
  std::vector<RegistrationObjectExtent> Objects;
  std::map<int32_t, FrameValue> ObjectCells;
  std::map<va_t, SEHJumpCapture> JumpBuffers;
  std::map<std::pair<va_t, int>, BorrowedJumpResume> JumpResumes;
  std::set<std::pair<int32_t, int32_t>> Reads;
  std::set<std::pair<int32_t, int32_t>> Writes;
  std::set<int32_t> UsedArguments;
  bool NativeFramePrivate = true;
  bool Complete = true;
  size_t Work = 0;
};

struct BoundCalleeEffect {
  uint32_t StackPopBytes = 0;
  std::vector<RegistrationObjectExtent> Reads;
  std::vector<RegistrationObjectExtent> Writes;
  bool HasOrdinaryReturn = true;
  std::vector<BorrowedJumpResume> JumpResumes;
  std::optional<RegistrationSEHBoundCall> NativeCall;
};

/// Record native frame observations without weakening the source state proof.
void checkConditionalNativeFrame(const FrameState &State, const LowOp &Op,
                                 const FrameTransfer &Transfer,
                                 ConditionalFrame &Inputs);

/// Prove one exact original helper using its current caller cells. The caller
/// separately binds this target to its authenticated physical CALL occurrence.
std::optional<BoundCalleeEffect> proveBoundSEHCallee(
    const BinaryImage &Image, const LowOp &Op, const FrameState &Parent,
    size_t &Work, const std::map<int32_t, SEHJumpCapture> *Captures = nullptr);

using BoundCalleeProver = std::function<std::optional<BoundCalleeEffect>(
    const LowOp &, const FrameState &, size_t &,
    const std::map<int32_t, SEHJumpCapture> &)>;

} // namespace neverd::registration_state

#endif
