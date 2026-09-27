//===- SymExprCompare.h - Bounded comparison recovery -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LIB_SYMBOLIC_EXPR_SYMEXPRCOMPARE_H
#define NEVERD_LIB_SYMBOLIC_EXPR_SYMEXPRCOMPARE_H

#include "neverd/symbolic/SymExpr.h"

namespace neverd::symbolic::detail {

/// Recover a predicate for the highest bit of a word. A failed match does not
/// intern nodes; the original full-width word always retains its meaning.
SymRef recoverSignComparison(SymContext &Ctx, SymRef Word);

/// Recover a small bitwise combination of highest-bit observations. Constant
/// is the canonical accumulated literal; operands contain the remaining terms.
SymRef recoverObservedComparison(SymContext &Ctx, SymOp Op,
                                 llvm::ArrayRef<SymRef> Operands,
                                 const llvm::APInt &Constant);

} // namespace neverd::symbolic::detail

#endif
