//===- X86RegistrationSEHJump.h - Source jump contracts -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checked source capture and direct-resume calls, independent of LLVM IR.

#ifndef NEVERD_IR_MED_X86REGISTRATIONSEHJUMP_H
#define NEVERD_IR_MED_X86REGISTRATIONSEHJUMP_H

#include "neverd/ir/RegistrationState.h"

#include <map>
#include <optional>

namespace neverd {
struct MedFunc;
struct MedOp;

struct X86RegistrationSEHJumpCall {
  const RegistrationSEHJumpCapture *Capture = nullptr;
  const RegistrationSEHJumpResume *Resume = nullptr;
  const MedOp *Operation = nullptr;
  int Block = -1;
  int32_t ResumeLevel = -1;

  /// The checked EH3 capture has two fixed words and two unwind arguments.
  /// A direct resume consumes only its buffer and scalar value. Neither takes
  /// register arguments; current SSA stack stores still own argument values.
  unsigned stackArgumentWords() const { return Resume ? 2 : 4; }
};

/// Bind complete source buffer lifetimes to exact MedIR call occurrences.
/// A native buffer and the generated capture state must be checked separately.
std::optional<std::map<va_t, X86RegistrationSEHJumpCall>>
getX86RegistrationSEHJumpCalls(const MedFunc &Function);

} // namespace neverd

#endif // NEVERD_IR_MED_X86REGISTRATIONSEHJUMP_H
