//===- OrderedQuery.h - Bounded complete-query ordering retry -----*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_ORDEREDQUERY_H
#define NEVERD_ANALYSIS_ORDEREDQUERY_H

#include "neverd/solver/BitVectorSolver.h"

#include "llvm/ADT/STLFunctionalExtras.h"

namespace neverd::analysis::detail {

inline bool isOriginalQueryConjunct(const symbolic::SymContext &Ctx,
                                    symbolic::SymRef Predicate,
                                    symbolic::SymRef First) {
  if (!Predicate || !First || Predicate.index() >= Ctx.numNodes() ||
      First.index() >= Ctx.numNodes() || Ctx.width(Predicate) != 1 ||
      Ctx.width(First) != 1 || Ctx.isConst(First) ||
      Ctx.op(Predicate) != symbolic::SymOp::And)
    return false;
  // Optional ordering must not introduce a traversal proportional to the DAG.
  if (Ctx.numOperands(Predicate) > 64)
    return false;
  for (const auto Operand : Ctx.operands(Predicate))
    if (Operand == First)
      return true;
  return false;
}

/// Check the full original question without constructing a model. The caller
/// owns both references, charges the initial query and enforces its node
/// bounds. ChargeRetry must charge the same global query allowance, or
/// interrupt before returning. No search is performed on a partial conjunction
/// or with relaxed options.
inline solver::SatResult
checkWithOrderedConjunct(symbolic::SymContext &Ctx, symbolic::SymRef Predicate,
                         symbolic::SymRef First, solver::SolverOptions Options,
                         llvm::function_ref<void()> ChargeRetry) {
  Options.BuildModel = false;
  solver::SatResult Answer;
  solver::BlastError Error;
  {
    solver::BitVectorSolver Original(Ctx, Options);
    Original.assertTrue(Predicate);
    Answer = Original.check();
    Error = Original.encodeError();
  }
  if (Answer != solver::SatResult::Unknown ||
      Error != solver::BlastError::None ||
      !isOriginalQueryConjunct(Ctx, Predicate, First))
    return Answer;

  ChargeRetry();
  solver::BitVectorSolver Retry(Ctx, Options);
  Retry.assertTrue(First);
  Retry.assertTrue(Predicate);
  return Retry.check();
}

} // namespace neverd::analysis::detail

#endif // NEVERD_ANALYSIS_ORDEREDQUERY_H
