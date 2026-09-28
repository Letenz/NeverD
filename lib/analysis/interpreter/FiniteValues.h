//===- FiniteValues.h - Bounded exhaustive bitvector projection ---*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_INTERPRETER_FINITEVALUES_H
#define NEVERD_ANALYSIS_INTERPRETER_FINITEVALUES_H

#include "neverd/analysis/InterpreterSpecialization.h"

namespace neverd::analysis::detail {

enum class FiniteValueStatus {
  Complete,
  TooManyValues,
  Unknown,
  Invalid,
  QueryBudgetExceeded,
};

struct FiniteValues {
  FiniteValueStatus Status = FiniteValueStatus::Unknown;
  /// Empty on every incomplete result. Complete with no tuples means the
  /// predicate is unsatisfiable, not that its outputs may take any value.
  std::vector<std::vector<uint64_t>> Tuples;
};

/// True only when Value is one variable whose type has more than Limit values
/// and whose exact symbol does not occur in Predicate. This does not establish
/// that Predicate is satisfiable: optional projection may omit the field, but
/// reachability still needs its own proof. An incomplete bounded DAG walk
/// returns false. The walk neither creates expressions nor invokes a solver.
bool hasUnconstrainedProjectionInput(const symbolic::SymContext &Ctx,
                                     symbolic::SymRef Predicate,
                                     symbolic::SymRef Value, uint32_t Limit,
                                     uint64_t MaxVisited);

FiniteValues
enumerateFiniteValues(symbolic::SymContext &Ctx, symbolic::SymRef Predicate,
                      llvm::ArrayRef<symbolic::SymRef> Values, uint32_t Limit,
                      const SpecializationOptions &Options, uint64_t &Queries);

} // namespace neverd::analysis::detail

#endif // NEVERD_ANALYSIS_INTERPRETER_FINITEVALUES_H
