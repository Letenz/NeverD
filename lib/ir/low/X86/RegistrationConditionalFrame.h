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

#include "neverd/ir/RegistrationCall.h"

#include <functional>
#include <map>
#include <set>

namespace neverd {
struct BinaryImage;
} // namespace neverd

namespace neverd::registration_state {

/// Invocation-specific inputs in the callee's established EBP coordinates.
/// Positive object extents are proved caller storage, disjoint from this
/// invocation's return PC, private frame and incoming argument slots.
struct ConditionalFrame {
  std::function<bool(const LowOp &, const FrameState &)> ProveCall;
  std::map<int32_t, FrameValue> Arguments;
  std::vector<RegistrationObjectExtent> Objects;
  std::map<int32_t, FrameValue> ObjectCells;
  std::set<std::pair<int32_t, int32_t>> Reads;
  std::set<std::pair<int32_t, int32_t>> Writes;
  bool Complete = true;
  size_t Work = 0;
};

struct BoundCalleeEffect {
  uint32_t StackPopBytes = 0;
  std::vector<RegistrationObjectExtent> Reads;
  std::vector<RegistrationObjectExtent> Writes;
};

/// Prove one exact original helper using its current caller cells. The caller
/// separately binds this target to its authenticated physical CALL occurrence.
std::optional<BoundCalleeEffect> proveBoundSEHCallee(const BinaryImage &Image,
                                                     const LowOp &Op,
                                                     const FrameState &Parent,
                                                     size_t &Work);

using BoundCalleeProver = std::function<std::optional<BoundCalleeEffect>(
    const LowOp &, const FrameState &, size_t &)>;

} // namespace neverd::registration_state

#endif
