//===- RegistrationJumpRuntime.h - PE32 jump runtime identities -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate registration-aware jump routines without inferring effects.

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONJUMPRUNTIME_H
#define NEVERD_IR_LOW_X86_REGISTRATIONJUMPRUNTIME_H

#include "neverd/Common.h"

#include <optional>

namespace neverd {
struct BinaryImage;

namespace registration_state {

struct JumpRuntimeBinding {
  enum class Kind { Capture, Resume, EH3Unwind } Routine;
  va_t Target = InvalidVA;
  va_t ImportSlot = InvalidVA;
  bool Indirect = false;
};

/// An exact non-delay PE32 runtime import, reached directly through its slot
/// or an immutable FF25 veneer. This grants no stack, memory, lifetime or
/// resumption authority; the invocation must separately bind its buffer.
std::optional<JumpRuntimeBinding>
checkedJumpRuntime(const BinaryImage &Image, va_t Target, bool Indirect);

} // namespace registration_state
} // namespace neverd

#endif
