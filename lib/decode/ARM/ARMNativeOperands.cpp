//===- ARMNativeOperands.cpp - Typed ARM native operands -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Describe ARM and Thumb immediates as 32-bit operands.
///
//===----------------------------------------------------------------------===//

#include "../NativeOperandsDetail.h"

#include "capstone/capstone.h"

namespace neverd::native_operands_detail {

void addARMImmediates(const DecodedInsn &I, NativeOperands &Result) {
  const auto &Detail = I.Raw->detail->arm;
  for (unsigned N = 0; N < Detail.op_count; ++N)
    if (Detail.operands[N].type == ARM_OP_IMM)
      addImmediate(Result, N, Detail.operands[N].imm, 32);
}

} // namespace neverd::native_operands_detail
