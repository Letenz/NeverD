//===- SymPrintShape.h - Shared expression spelling decisions --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SYMBOLIC_SYMPRINTSHAPE_H
#define NEVERD_SYMBOLIC_SYMPRINTSHAPE_H

#include "neverd/symbolic/SymExpr.h"

namespace neverd::symbolic::print_detail {

inline bool hasSignedMagnitude(const llvm::APInt &Value) {
  return Value.getBitWidth() > 1 && Value.isNegative() &&
         !Value.isMinSignedValue();
}

/// Whether a term can put its sign on a subtraction in a surrounding sum.
inline bool hasLeadingMinus(const SymContext &Ctx, SymRef R) {
  if (Ctx.isConst(R))
    return hasSignedMagnitude(Ctx.constValue(R));
  if (Ctx.op(R) != SymOp::Mul)
    return false;
  SymRef First = Ctx.operand(R, 0);
  return Ctx.isConst(First) && hasSignedMagnitude(Ctx.constValue(First));
}

/// A unit negative coefficient prints as unary negation, without a literal.
inline bool hasUnaryMinus(const SymContext &Ctx, SymRef R) {
  return Ctx.op(R) == SymOp::Mul && Ctx.width(R) > 1 &&
         Ctx.isConstOnes(Ctx.operand(R, 0));
}

inline unsigned leadingAddOperand(const SymContext &Ctx,
                                  llvm::ArrayRef<SymRef> Ops) {
  // Literal prefixes retain their ordering. Otherwise a positive term can
  // absorb a preceding product's unary sign into binary subtraction.
  if (!Ctx.isConst(Ops[0]) && hasLeadingMinus(Ctx, Ops[0]))
    for (unsigned I = 1; I < Ops.size(); ++I)
      if (!hasLeadingMinus(Ctx, Ops[I]))
        return I;
  return 0;
}

} // namespace neverd::symbolic::print_detail

#endif // NEVERD_SYMBOLIC_SYMPRINTSHAPE_H
