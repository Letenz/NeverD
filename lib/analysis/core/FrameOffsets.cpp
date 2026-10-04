//===- FrameOffsets.cpp - Proved entry-relative addresses ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "FrameOffsets.h"

#include "ControlDiscovery.h"

namespace neverd::analysis::detail {

using namespace symbolic;

FrameOffset proveFrameOffset(SymContext &Ctx, SymRef Predicate, SymRef Value,
                             SymRef Root, solver::SolverOptions Settings,
                             uint64_t MaxQueries, uint64_t MaxSymbolicNodes,
                             uint64_t &Queries, FiniteQueryCache *Cache) {
  const auto Valid = [&](SymRef R, unsigned Width) {
    return R && R.index() < Ctx.numNodes() && Ctx.width(R) == Width;
  };
  if (!Valid(Predicate, 1) || !Valid(Value, 64) || !Valid(Root, 64))
    return {FrameOffsetStatus::Invalid};
  if (Ctx.numNodes() > MaxSymbolicNodes)
    return {FrameOffsetStatus::BudgetExceeded};
  if (Ctx.isConstZero(Predicate))
    return {FrameOffsetStatus::Infeasible};
  if (const auto Offset = frameRelativeOffset(Ctx, Value, Root))
    return {FrameOffsetStatus::Exact, *Offset};
  const SymRef Difference = Ctx.mkSub(Value, Root);
  if (Ctx.numNodes() > MaxSymbolicNodes)
    return {FrameOffsetStatus::BudgetExceeded};
  if (const auto Constant = Ctx.asConst(Difference))
    return {FrameOffsetStatus::Exact, Constant->getZExtValue()};

  auto Cached =
      Cache ? Cache->lookup(Ctx, Predicate, {Difference}, 1) : std::nullopt;
  const auto Domain =
      Cached ? std::move(*Cached)
             : enumerateFiniteValues(Ctx, Predicate, {Difference}, 1, Settings,
                                     MaxQueries, MaxSymbolicNodes, Queries);
  if (Ctx.numNodes() > MaxSymbolicNodes)
    return {FrameOffsetStatus::BudgetExceeded};
  if (Cache && !Cached)
    Cache->store(Ctx, Predicate, {Difference}, 1, Domain);
  switch (Domain.Status) {
  case FiniteValueStatus::Complete:
    if (Domain.Tuples.empty())
      return {FrameOffsetStatus::Infeasible};
    if (Domain.Tuples.size() != 1 || Domain.Tuples.front().size() != 1)
      return {FrameOffsetStatus::Invalid};
    return {FrameOffsetStatus::Exact, Domain.Tuples.front().front()};
  case FiniteValueStatus::TooManyValues:
    return {FrameOffsetStatus::NonUnique};
  case FiniteValueStatus::Invalid:
    return {FrameOffsetStatus::Invalid};
  case FiniteValueStatus::Unknown:
  case FiniteValueStatus::QueryBudgetExceeded:
    return {FrameOffsetStatus::BudgetExceeded};
  }
  llvm_unreachable("invalid finite value status");
}

FrameOffset proveFrameOffset(SymContext &Ctx, SymRef Predicate, SymRef Value,
                             SymRef Root, const SpecializationOptions &Options,
                             uint64_t &Queries, FiniteQueryCache *Cache) {
  solver::SolverOptions Settings;
  Settings.Blast.MaxGates = Options.MaxSolverGates;
  Settings.Sat.MaxConflicts = Options.MaxSolverConflicts;
  Settings.Sat.MaxPropagations = Options.MaxSolverPropagations;
  Settings.Sat.MaxWatchVisits = Options.MaxSolverWatchVisits;
  return proveFrameOffset(Ctx, Predicate, Value, Root, Settings,
                          Options.MaxSolverQueries, Options.MaxSymbolicNodes,
                          Queries, Cache);
}

} // namespace neverd::analysis::detail
