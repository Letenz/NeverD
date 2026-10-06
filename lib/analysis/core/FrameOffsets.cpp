//===- FrameOffsets.cpp - Proved entry-relative addresses ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "FrameOffsets.h"

#include "ControlDiscovery.h"

#include <optional>
#include <utility>

namespace neverd::analysis::detail {

using namespace symbolic;

namespace {

std::optional<std::pair<SymRef, llvm::APInt>>
constantOperand(const SymContext &Ctx, SymRef Value, SymOp Op) {
  if (Ctx.op(Value) != Op || Ctx.numOperands(Value) != 2)
    return std::nullopt;
  for (unsigned I = 0; I != 2; ++I) {
    const auto C = Ctx.operand(Value, I);
    if (const auto Number = Ctx.asConst(C))
      return std::pair{Ctx.operand(Value, 1 - I), *Number};
  }
  return std::nullopt;
}

// Derive Value - Root without subtracting two full-width copies of Root.
// These are modular identities, independent of the path predicate. In
// particular, (X & Mask) - Root == (X - Root) - (X & ~Mask). Alignment then
// projects only the removed low bits; the solver still proves their complete
// domain under the original predicate. No residue or feasibility is assumed.
// Only binary operations and one matching split are inspected per visit. The
// shared visit budget bounds inspection; the caller charges every created node.
std::optional<SymRef> relativeRemainder(SymContext &Ctx, SymRef Value,
                                        SymRef Root, unsigned &Remaining) {
  if (!Remaining || Ctx.width(Value) != 64)
    return std::nullopt;
  --Remaining;
  if (Value == Root)
    return Ctx.mkZero(64);
  if (const auto Add = constantOperand(Ctx, Value, SymOp::Add)) {
    if (const auto Inner = relativeRemainder(Ctx, Add->first, Root, Remaining))
      return Ctx.mkAdd(*Inner, Ctx.mkConst(Add->second));
    return std::nullopt;
  }
  // (Base + Index) - Root == (Base - Root) + Index. The other operand
  // remains symbolic, including any dependence on Root or on the predicate.
  // Share the inspection budget across both choices to bound branching work.
  if (Ctx.op(Value) == SymOp::Add && Ctx.numOperands(Value) == 2) {
    for (unsigned I = 0; I != 2; ++I) {
      const auto Base = Ctx.operand(Value, I);
      const auto Index = Ctx.operand(Value, 1 - I);
      if (const auto Inner = relativeRemainder(Ctx, Base, Root, Remaining))
        return Ctx.mkAdd(*Inner, Index);
    }
    return std::nullopt;
  }
  const auto Masked = [&](SymRef Base,
                          const llvm::APInt &Mask) -> std::optional<SymRef> {
    const auto Inner = relativeRemainder(Ctx, Base, Root, Remaining);
    if (!Inner)
      return std::nullopt;
    const auto RemovedMask = ~Mask;
    const auto Bits = RemovedMask.getActiveBits();
    if (!Bits)
      return Inner;
    const auto Removed = Ctx.mkAnd(Ctx.mkExtract(Base, 0, Bits),
                                   Ctx.mkConst(RemovedMask.zextOrTrunc(Bits)));
    return Ctx.mkSub(*Inner, Ctx.mkZExtOrTrunc(Removed, 64));
  };
  if (const auto And = constantOperand(Ctx, Value, SymOp::And))
    return Masked(And->first, And->second);
  if (Ctx.op(Value) != SymOp::Concat || Ctx.numOperands(Value) != 2)
    return std::nullopt;
  const auto High = Ctx.operand(Value, 0), Low = Ctx.operand(Value, 1);
  const auto Bits = Ctx.width(Low);
  if (!Bits || Bits >= 64 || Ctx.width(High) != 64 - Bits ||
      Ctx.op(High) != SymOp::Extract || Ctx.numOperands(High) != 1 ||
      Ctx.node(High).Aux != Bits)
    return std::nullopt;
  const auto Base = Ctx.operand(High, 0);
  if (Ctx.width(Base) != 64)
    return std::nullopt;
  const auto HighMask = ~llvm::APInt::getLowBitsSet(64, Bits);
  if (Ctx.isConstZero(Low))
    return Masked(Base, HighMask);
  if (const auto And = constantOperand(Ctx, Low, SymOp::And)) {
    // mkExtract applies the same narrow arithmetic canonicalization that the
    // original partial-register expression used. Different roots or biases
    // must never be stitched together merely because their widths agree.
    if (And->first == Ctx.mkExtract(Base, 0, Bits))
      return Masked(Base, HighMask | And->second.zext(64));
  }
  return std::nullopt;
}

} // namespace

static FrameOffset
proveFrameOffsetImpl(SymContext &Ctx, SymRef Predicate, SymRef Value,
                     SymRef Root, solver::SolverOptions Settings,
                     uint64_t MaxQueries, uint64_t MaxSymbolicNodes,
                     uint64_t &Queries, FiniteQueryCache *Cache,
                     FiniteDomainEncoding *Encoding) {
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
  unsigned Remaining = 16;
  const auto Remainder = relativeRemainder(Ctx, Value, Root, Remaining);
  const SymRef Difference = Remainder ? *Remainder : Ctx.mkSub(Value, Root);
  if (Ctx.numNodes() > MaxSymbolicNodes)
    return {FrameOffsetStatus::BudgetExceeded};
  if (const auto Constant = Ctx.asConst(Difference))
    return {FrameOffsetStatus::Exact, Constant->getZExtValue()};

  auto Prepared = Cache ? Cache->prepare(Ctx, Predicate, {Difference}, 1)
                        : FiniteQueryCache::PreparedQuery{};
  auto Cached = Cache ? Cache->lookup(Prepared) : std::nullopt;
  const auto Domain =
      Cached ? std::move(*Cached)
      : Encoding
          ? enumerateFiniteValues(*Encoding, Predicate, {Difference}, 1,
                                  MaxQueries, MaxSymbolicNodes, Queries)
          : enumerateFiniteValues(Ctx, Predicate, {Difference}, 1, Settings,
                                  MaxQueries, MaxSymbolicNodes, Queries);
  if (Ctx.numNodes() > MaxSymbolicNodes)
    return {FrameOffsetStatus::BudgetExceeded};
  if (Cache && !Cached)
    Cache->store(std::move(Prepared), Domain);
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
                             SymRef Root, solver::SolverOptions Settings,
                             uint64_t MaxQueries, uint64_t MaxSymbolicNodes,
                             uint64_t &Queries, FiniteQueryCache *Cache) {
  return proveFrameOffsetImpl(Ctx, Predicate, Value, Root, Settings, MaxQueries,
                              MaxSymbolicNodes, Queries, Cache, nullptr);
}

FrameOffset proveFrameOffset(FiniteDomainEncoding &Encoding, SymRef Predicate,
                             SymRef Value, SymRef Root, uint64_t MaxQueries,
                             uint64_t MaxSymbolicNodes, uint64_t &Queries,
                             FiniteQueryCache *Cache) {
  return proveFrameOffsetImpl(Encoding.context(), Predicate, Value, Root,
                              Encoding.settings(), MaxQueries, MaxSymbolicNodes,
                              Queries, Cache, &Encoding);
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
