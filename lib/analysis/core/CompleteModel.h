//===- CompleteModel.h - Complete concrete witness validation ===========//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_COMPLETEMODEL_H
#define NEVERD_ANALYSIS_COMPLETEMODEL_H

#include "neverd/solver/BitVectorSolver.h"

namespace neverd::analysis::complete_model {
enum class Verdict {
  Satisfied,
  Refuted,
  MissingVariable,
  Invalid,
  BudgetExceeded
};
struct Result {
  Verdict Answer = Verdict::Invalid;
  uint64_t Nodes = 0, Edges = 0, Variables = 0, Words = 0;
};

// Satisfied certifies only this complete concrete assignment. Refuted does not
// prove the predicate unsatisfiable. Every required variable has its exact
// width.
Result verify(const symbolic::SymContext &Ctx, symbolic::SymRef Predicate,
              const solver::BitVectorModel &Model, uint64_t MaxWords,
              uint32_t MaxWidth);
} // namespace neverd::analysis::complete_model
#endif
