//===- SymExprMask.h - Local word-mask reasoning -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_SYMBOLIC_EXPR_SYMEXPRMASK_H
#define NEVERD_LIB_SYMBOLIC_EXPR_SYMEXPRMASK_H

#include "neverd/symbolic/SymExpr.h"

#include <optional>

namespace neverd::symbolic::detail {

/// Match a power-of-two multiplier against the numeric value of a constant
/// shift count, without truncating an independently sized count.
std::optional<unsigned> matchingShiftPower(const SymContext &Ctx, SymRef Count,
                                           const llvm::APInt &Factor);

} // namespace neverd::symbolic::detail

#endif
