//===- X86RegistrationFrame.h - Runtime root expressions --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_X86REGISTRATIONFRAME_H
#define NEVERD_IR_HIGH_X86REGISTRATIONFRAME_H

#include "neverd/ir/high/HighIR.h"

namespace neverd {

ExprPtr lowerX86RegistrationRoot(const MedFunc &Func, const MedOp &Op);

} // namespace neverd

#endif // NEVERD_IR_HIGH_X86REGISTRATIONFRAME_H
