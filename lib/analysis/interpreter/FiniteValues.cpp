//===- FiniteValues.cpp - Bounded exhaustive bitvector projection ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "FiniteValues.h"

#include "neverd/solver/BitVectorSolver.h"

#include "llvm/ADT/DenseSet.h"

#include <algorithm>

namespace neverd::analysis::detail {

using namespace symbolic;
using namespace solver;

bool hasUnconstrainedProjectionInput(const SymContext &Ctx, SymRef Predicate,
                                     SymRef Value, uint32_t Limit,
                                     uint64_t MaxVisited) {
  if (!Predicate || Ctx.width(Predicate) != 1 || !Value || !Ctx.isVar(Value) ||
      !Ctx.width(Value) || !Limit || !MaxVisited)
    return false;
  if (Ctx.width(Value) < 64 && (uint64_t{1} << Ctx.width(Value)) <= Limit)
    return false;

  llvm::DenseSet<uint32_t> Seen;
  llvm::SmallVector<SymRef, 16> Pending{Predicate};
  Seen.insert(Predicate.index());
  while (!Pending.empty()) {
    const SymRef Current = Pending.pop_back_val();
    if (Current == Value)
      return false;
    for (SymRef Operand : Ctx.operands(Current)) {
      if (Seen.contains(Operand.index()))
        continue;
      // Charge unique nodes when queued, bounding both the walk and storage
      // even when a node has many operands or the expression is a shared DAG.
      if (Seen.size() >= MaxVisited)
        return false;
      Seen.insert(Operand.index());
      Pending.push_back(Operand);
    }
  }
  return true;
}

FiniteValues enumerateFiniteValues(SymContext &Ctx, SymRef Predicate,
                                   llvm::ArrayRef<SymRef> Values,
                                   uint32_t Limit,
                                   const SpecializationOptions &Options,
                                   uint64_t &Queries) {
  if (!Predicate || Ctx.width(Predicate) != 1 || !Limit)
    return {FiniteValueStatus::Invalid, {}};
  for (SymRef Value : Values)
    if (!Value || !Ctx.width(Value) || Ctx.width(Value) > 64)
      return {FiniteValueStatus::Invalid, {}};
  if (Ctx.isConstZero(Predicate))
    return {FiniteValueStatus::Complete, {}};
  if (Ctx.isConstOnes(Predicate)) {
    // A free wide input alone already exceeds this projection's value limit.
    // Avoid spending solver queries on ordinary unconstrained data pointers.
    for (SymRef Value : Values)
      if (Ctx.isVar(Value) &&
          (Ctx.width(Value) >= 64 || (uint64_t{1} << Ctx.width(Value)) > Limit))
        return {FiniteValueStatus::TooManyValues, {}};
    std::vector<uint64_t> Tuple;
    for (SymRef Value : Values) {
      const auto Constant = Ctx.asConst(Value);
      if (!Constant)
        break;
      Tuple.push_back(Constant->getZExtValue());
    }
    if (Tuple.size() == Values.size())
      return {FiniteValueStatus::Complete, {std::move(Tuple)}};
  }
  if (Ctx.numNodes() > Options.MaxSymbolicNodes)
    return {FiniteValueStatus::Unknown, {}};

  SolverOptions Settings;
  Settings.Blast.MaxGates = Options.MaxSolverGates;
  Settings.Sat.MaxConflicts = Options.MaxSolverConflicts;
  Settings.Sat.MaxPropagations = Options.MaxSolverPropagations;
  Settings.Sat.MaxWatchVisits = Options.MaxSolverWatchVisits;
  BitVectorSolver Solver(Ctx, Settings);
  const auto EncodingFailure = [&] {
    return FiniteValues{Solver.encodeError() == BlastError::Malformed
                            ? FiniteValueStatus::Invalid
                            : FiniteValueStatus::Unknown,
                        {}};
  };
  if (!Solver.assertTrue(Predicate))
    return EncodingFailure();

  // Explicit query variables force every requested value into the model.
  // Never fill a missing model value with zero or enumerate one operand at a
  // time: the joint model preserves correlations among all requested values.
  llvm::SmallVector<SymRef, 8> QueryValues;
  for (SymRef Value : Values) {
    SymRef Query = Ctx.mkFreshVar(Ctx.width(Value), "finite_value");
    QueryValues.push_back(Query);
    if (!Solver.assertEqual(Query, Value))
      return EncodingFailure();
  }
  FiniteValues Result;
  while (true) {
    if (Queries >= Options.MaxSolverQueries)
      return {FiniteValueStatus::QueryBudgetExceeded, {}};
    if (Ctx.numNodes() > Options.MaxSymbolicNodes)
      return {FiniteValueStatus::Unknown, {}};
    ++Queries;
    switch (Solver.check()) {
    case SatResult::Unsat:
      Result.Status = FiniteValueStatus::Complete;
      std::sort(Result.Tuples.begin(), Result.Tuples.end());
      return Result;
    case SatResult::Unknown:
      return {FiniteValueStatus::Unknown, {}};
    case SatResult::Invalid:
      return {FiniteValueStatus::Invalid, {}};
    case SatResult::Sat:
      break;
    }
    if (Result.Tuples.size() >= Limit)
      return {FiniteValueStatus::TooManyValues, {}};
    std::vector<uint64_t> Tuple;
    llvm::SmallVector<SymRef, 8> Different;
    for (SymRef Query : QueryValues) {
      const auto Value = Solver.model().value(Ctx, Query);
      if (!Value)
        return {FiniteValueStatus::Invalid, {}};
      Tuple.push_back(Value->getZExtValue());
      Different.push_back(Ctx.mkNot(Ctx.mkEq(Query, Ctx.mkConst(*Value))));
    }
    Result.Tuples.push_back(std::move(Tuple));
    // Empty projection has one possible tuple; blocking it proves that no
    // further tuple exists. A final UNSAT check is still required.
    if (!Solver.assertTrue(Different.empty() ? Ctx.mkFalse()
                                             : Ctx.mkOr(Different)))
      return EncodingFailure();
  }
}

} // namespace neverd::analysis::detail
