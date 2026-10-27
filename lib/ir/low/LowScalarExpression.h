//===- LowScalarExpression.h - Scalar identities ------------*- C++ -*-===//
//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Prove a numeric result independent of a block's unknown scalar inputs.

#ifndef NEVERD_LIB_IR_LOW_LOWSCALAREXPRESSION_H
#define NEVERD_LIB_IR_LOW_LOWSCALAREXPRESSION_H

#include "neverd/ir/low/LowIR.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace neverd {

/// Operations precede the query in one straight basic block. No predecessor,
/// memory contents or path assumptions supply values. Unknown writes receive
/// distinct inputs; register bytes overlap and temporaries are instruction
/// local. Only exact symbolic derivations may publish a constant. The caller
/// separately authenticates its address role and revalidates changed blocks.
std::optional<uint64_t>
constantScalarExpression(llvm::ArrayRef<LowOp> Operations, const NdVar &Value,
                         va_t QueryInstruction, size_t &RemainingWork);

} // namespace neverd

#endif // NEVERD_LIB_IR_LOW_LOWSCALAREXPRESSION_H
