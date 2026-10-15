//===- X86RegistrationCallResult.h - PE32 result uses ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check one current SSA occurrence before discarding its physical result.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_MED_X86REGISTRATIONCALLRESULT_H
#define NEVERD_IR_MED_X86REGISTRATIONCALLRESULT_H

#include <cstddef>

namespace neverd {
struct MedFunc;
struct RegistrationCallFrameEffect;

/// Follow only SSA copies, integer extensions and PHIs. Every other use,
/// including a call's implicit arguments, observes the result. Duplicate or
/// missing definitions, ambiguous occurrences and exhausted budgets reject.
/// This proves absence of a source observation, never a language return type.
bool hasUnusedX86RegistrationCallResult(const MedFunc &Function,
                                        const RegistrationCallFrameEffect &Call,
                                        size_t &Work);
} // namespace neverd

#endif
