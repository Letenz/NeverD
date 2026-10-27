//===- RegistrationSEHRuntimeReturn.h - Unobserved returns ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Binds returns whose scalar value is unobserved by every finally entry.

#ifndef NEVERD_IR_MED_X86_REGISTRATIONSEHRUNTIMERETURN_H
#define NEVERD_IR_MED_X86_REGISTRATIONSEHRUNTIMERETURN_H

#include "neverd/Common.h"

#include <cstddef>
#include <optional>
#include <set>

namespace neverd {
struct MedFunc;
struct MedOp;

std::optional<std::set<const MedOp *>>
getRuntimeSEHFinallyReturns(const MedFunc &Function, const MedOp &Call,
                            va_t Entry, size_t &Work);
} // namespace neverd

#endif // NEVERD_IR_MED_X86_REGISTRATIONSEHRUNTIMERETURN_H
