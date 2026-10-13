//===- LowScalarConstant.h - Exact integer constant results ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Evaluates fixed-width scalar LowIR operations from checked input facts.

#ifndef NEVERD_IR_LOW_LOWSCALARCONSTANT_H
#define NEVERD_IR_LOW_LOWSCALARCONSTANT_H

#include "neverd/ir/low/LowIR.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <optional>

namespace neverd {

/// Evaluate the bounded scalar operations shared by exact frame-value
/// propagation and image-address inventories. Results wrap at the declared
/// width; a pointer identity, a memory value or an unsupported operation is
/// never supplied by this query. The caller owns the reaching operand facts.
std::optional<uint64_t> lowScalarConstantResult(
    const LowOp &Operation,
    llvm::function_ref<std::optional<uint64_t>(const NdVar &)> ValueOf);

} // namespace neverd

#endif
