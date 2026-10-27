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

#include "llvm/ADT/ArrayRef.h"

#include <optional>

namespace neverd {
struct BinaryImage;
struct LowFunc;
struct LowOp;

namespace registration_state {

struct JumpRuntimeBinding {
  enum class Kind { Capture, Resume, EH3Unwind, EH4Unwind } Routine;
  va_t Target = InvalidVA;
  va_t ImportSlot = InvalidVA;
  bool Indirect = false;
};

/// An exact non-delay PE32 runtime import, reached directly through its slot
/// or an immutable FF25 veneer. This grants no stack, memory, lifetime or
/// resumption authority; the invocation must separately bind its buffer.
std::optional<JumpRuntimeBinding>
checkedJumpRuntime(const BinaryImage &Image, va_t Target, bool Indirect);

/// Bind an original direct E8 occurrence to the authenticated import. A
/// detached operation or altered instruction slice grants no call authority.
std::optional<JumpRuntimeBinding>
checkedJumpRuntimeCall(const BinaryImage &Image, const LowFunc &Function,
                       const LowOp &Op, size_t &Work);

/// Partition the ordinary fallthrough at an authenticated capture or resume
/// call. This grants no buffer, frame, resumption or no-return authority.
bool isSEHJumpRuntimeInstruction(const BinaryImage &Image, va_t Address,
                                 uint16_t Size, llvm::ArrayRef<LowOp> Ops);

} // namespace registration_state
} // namespace neverd

#endif
