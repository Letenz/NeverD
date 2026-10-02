//===- StringTransfer.h - Bounded repeated memory transfers -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_STRING_TRANSFER_H
#define NEVERD_ANALYSIS_STRING_TRANSFER_H

#include "neverd/ir/low/LowIR.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

namespace neverd::analysis::detail {

struct StringTransferShape {
  unsigned ElementBytes;
  unsigned CountInput;
  bool Fill;
};

/// Only exact 64-bit-address MOVS/STOS shapes are admitted. The lifter owns
/// the separate pointer/count updates; the intrinsic's discarded output is
/// zero, as in both source backends. Ordered and segmented accesses are not
/// equivalent to the ordinary scalar memory operations used here.
std::optional<StringTransferShape> stringTransferShape(const LowOp &Op);

/// Lower a caller-proved count and direction. Each element is read completely
/// before its write, and later iterations observe prior writes. This is not
/// memcpy/memmove. Zero count accesses no memory and ignores Backward.
///
/// All instruction temporaries must appear in InstructionOps. Scratch is
/// allocated after them and below TemporaryLimit. MaxOperations is checked
/// before allocation; the caller must still charge every emitted operation to
/// its shared budget. This routine proves no value, alias, or frame assumption.
llvm::Expected<std::vector<LowOp>>
lowerStringTransfer(const LowOp &Op, uint64_t Count, bool Backward,
                    llvm::ArrayRef<LowOp> InstructionOps,
                    uint64_t TemporaryLimit, uint64_t MaxOperations);

} // namespace neverd::analysis::detail

#endif
