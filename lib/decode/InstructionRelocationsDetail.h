//===- InstructionRelocationsDetail.h - Per-ISA relocation fields -*- C++ -*-//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The instruction-set specific part of instructionRelocations: operand
/// fields whose address or scalar role depends on the encoding of one ISA.
/// Each ISA lives in its own InstructionRelocations<ISA>.cpp.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_DECODE_INSTRUCTIONRELOCATIONSDETAIL_H
#define NEVERD_LIB_DECODE_INSTRUCTIONRELOCATIONSDETAIL_H

#include "neverd/decode/InstructionRelocations.h"

namespace neverd {
namespace instruction_relocations_detail {

/// i386 and x86-64 fields of \p DI (InstructionRelocationsX86.cpp).
void addX86Fields(const BinaryImage &Img, const DecodedInsn &DI,
                  InstructionRelocations &Result);

/// AArch64 fields of \p DI (InstructionRelocationsAArch64.cpp).
void addAArch64Fields(const BinaryImage &Img, const DecodedInsn &DI,
                      InstructionRelocations &Result);

} // namespace instruction_relocations_detail
} // namespace neverd

#endif // NEVERD_LIB_DECODE_INSTRUCTIONRELOCATIONSDETAIL_H
