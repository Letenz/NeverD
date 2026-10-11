//===- X86RegistrationFilter.h - Native x86 filter values ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_STRUCTURE_X86REGISTRATIONFILTER_H
#define NEVERD_IR_HIGH_STRUCTURE_X86REGISTRATIONFILTER_H

#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/med/MedIR.h"

namespace neverd {

/// A pure return expression in the authenticated parent-frame coordinate.
/// Failure retains the native thunk; it cannot authorize an ordinary C call.
ExprPtr registrationFilterValue(const std::vector<HighStmt> &Body,
                                const MedFunc &Med);

} // namespace neverd

#endif
