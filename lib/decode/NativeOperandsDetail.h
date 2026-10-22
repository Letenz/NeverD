//===- NativeOperandsDetail.h - Per-ISA native operands --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private helpers for native operand descriptions. Architecture-specific
/// operand interpretation lives in the corresponding ISA directory.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_DECODE_NATIVEOPERANDSDETAIL_H
#define NEVERD_LIB_DECODE_NATIVEOPERANDSDETAIL_H

#include "neverd/decode/NativeOperands.h"

#include <cstdint>

namespace neverd::native_operands_detail {

/// Append an immediate masked to its logical operand width, if representable.
void addImmediate(NativeOperands &Result, unsigned Index, int64_t Value,
                  unsigned Bits);

/// Architecture helpers require an instruction with non-null Raw and detail.
void addX86Immediates(const DecodedInsn &I, NativeOperands &Result);
void addAArch64Immediates(const Decoder &D, const DecodedInsn &I,
                          NativeOperands &Result);
void addARMImmediates(const DecodedInsn &I, NativeOperands &Result);

} // namespace neverd::native_operands_detail

#endif // NEVERD_LIB_DECODE_NATIVEOPERANDSDETAIL_H
