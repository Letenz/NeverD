//===- FloatConversion.h - Scalar FP conversion policy ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_FLOATCONVERSION_H
#define NEVERD_IR_FLOATCONVERSION_H

#include "neverd/Common.h"

namespace neverd {

enum class FPToIntegerPolicy { Saturate, X86Indefinite };

/// Result policy for scalar FLOAT_FLOAT2INT/FLOAT_FLOAT2UINT/FLOAT_TRUNC.
/// Floating control/status effects belong to their architecture intrinsics.
constexpr FPToIntegerPolicy fpToIntegerPolicy(Arch Target) {
  return Target == Arch::X86 || Target == Arch::X64
             ? FPToIntegerPolicy::X86Indefinite
             : FPToIntegerPolicy::Saturate;
}

} // namespace neverd

#endif
