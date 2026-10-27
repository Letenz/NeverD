//===- FrameOffsets.cpp - Proved entry-relative addresses ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "FrameOffsets.h"

#include "ControlDiscovery.h"

#include "neverd/analysis/Limits.h"
#include "neverd/symbolic/SymMBA.h"

#include <algorithm>
#include <cassert>
#include <limits>
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

std::optional<SymRef>
normalizeFrameExpression(SymContext &Ctx, SymRef Value,
                         uint64_t MaxSymbolicNodes,
                         FrameOffsetNormalization &Budget) {
  // Readability extends a context-owned prefix cache. Charge only its pending
  // extension: earlier queries or builders have already computed older costs.
  // Counts are fixed-width metadata, so pricing needs no operand-list walk or
  // constant-payload inspection. These are admission/measurement units, not an
  // upper bound on rebuilding, sorting or default sample defect guards.
  for (uint64_t I = Ctx.numCachedReadabilityNodes(), N = Ctx.numNodes(); I != N;
       ++I) {
    const SymRef R(static_cast<uint32_t>(I));
    // Operand counts and bit widths are 32-bit, so this sum cannot overflow.
    const uint64_t Cost =
        1 + uint64_t{Ctx.numOperands(R)} +
        (Ctx.op(R) == SymOp::Const ? (uint64_t{Ctx.width(R)} + 63) / 64 : 0);
    if (Cost > Budget.MaxWork - Budget.WorkUsed) {
      Budget.WorkUsed = Budget.MaxWork;
      return std::nullopt;
    }
    Budget.WorkUsed += Cost;
  }
  const auto Remaining = Budget.MaxWork - Budget.WorkUsed;
  if (!Remaining)
    return std::nullopt;
  MBAOptions Options;
  // Never interpret a large caller budget as MBA's unlimited sentinel.
  Options.MaxWork = static_cast<size_t>(
      std::min<uint64_t>(Remaining, std::numeric_limits<size_t>::max() - 1));
  const auto Result =
      Budget.Rewrites ? simplifyMBADeep(Ctx, Value, Options, *Budget.Rewrites)
                      : simplifyMBADeep(Ctx, Value, Options);
  Budget.WorkUsed += Result.Work;
  // The context bound gates publication, not peak allocation inside MBA.
  if (Ctx.numNodes() > MaxSymbolicNodes || !Result.Expr ||
      Result.Expr.index() >= Ctx.numNodes() || Ctx.width(Result.Expr) != 64 ||
      Result.Evidence != MBAEvidence::Derivation)
    return std::nullopt;
  return Result.Expr;
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

static FrameOffset proveFrameOffsetImpl(
    SymContext &Ctx, SymRef Predicate, SymRef Value, SymRef Root,
    solver::SolverOptions Settings, uint64_t MaxQueries,
    uint64_t MaxSymbolicNodes, uint64_t &Queries, FiniteQueryCache *Cache,
    FiniteDomainEncoding *Encoding, FrameOffsetNormalization *Normalization,
    FiniteValues *CompletedDomain = nullptr) {
  // Only the uncached normalization retry requests this evidence. Algebraic
  // offsets carry no feasible-domain witness and leave it unknown.
  assert(!CompletedDomain || !Cache);
  if (CompletedDomain)
    *CompletedDomain = {};
  if (Normalization)
    Normalization->WorkUsed = 0;
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
  unsigned Remaining =
      ::neverd::limits::analysis::kFrameOffsetsProveFrameOffsetImplRemaining;
  const auto Remainder = relativeRemainder(Ctx, Value, Root, Remaining);
  const SymRef Difference = Remainder ? *Remainder : Ctx.mkSub(Value, Root);
  if (Ctx.numNodes() > MaxSymbolicNodes)
    return {FrameOffsetStatus::BudgetExceeded};
  if (const auto Constant = Ctx.asConst(Difference))
    return {FrameOffsetStatus::Exact, Constant->getZExtValue()};

  // Value == CacheAddress + CacheBias modulo 2^64. Derive the cache key
  // from the original address, independently of the bounded remainder
  // rewrite used to make the cold proof small. Equivalent translated
  // addresses must not acquire unrelated keys when that rewrite chooses a
  // different shape. The actual cold solver query is still Difference.
  SymRef CacheAddress = Value;
  uint64_t CacheBias = 0;
  if (Cache && Ctx.op(Value) == SymOp::Add) {
    llvm::SmallVector<SymRef, 8> Terms;
    for (const auto Term : Ctx.operands(Value)) {
      if (const auto Number = Ctx.asConst(Term))
        CacheBias += Number->getZExtValue();
      else
        Terms.push_back(Term);
    }
    if (CacheBias && !Terms.empty())
      CacheAddress = Ctx.mkAdd(Terms);
  }
  const auto CacheValue = Cache ? Ctx.mkSub(CacheAddress, Root) : Difference;
  if (Ctx.numNodes() > MaxSymbolicNodes)
    return {FrameOffsetStatus::BudgetExceeded};

  auto Prepared = Cache ? Cache->prepare(Ctx, Predicate, {CacheValue}, 1)
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
  const auto RememberDomain = [&](const FiniteValues &Proved) {
    if (!Cache || Cached ||
        (Proved.Status != FiniteValueStatus::Complete &&
         Proved.Status != FiniteValueStatus::TooManyValues))
      return;
    if (CacheBias && Proved.Status == FiniteValueStatus::Complete) {
      auto Unbiased = Proved;
      for (auto &Tuple : Unbiased.Tuples)
        if (Tuple.size() == 1)
          Tuple.front() -= CacheBias;
      Cache->store(std::move(Prepared), Unbiased);
    } else {
      Cache->store(std::move(Prepared), Proved);
    }
  };
  RememberDomain(Domain);
  switch (Domain.Status) {
  case FiniteValueStatus::Complete:
    if (Domain.Tuples.empty()) {
      if (CompletedDomain)
        *CompletedDomain = Domain;
      return {FrameOffsetStatus::Infeasible};
    }
    if (Domain.Tuples.size() != 1 || Domain.Tuples.front().size() != 1)
      return {FrameOffsetStatus::Invalid};
    if (CompletedDomain)
      *CompletedDomain = Domain;
    return {FrameOffsetStatus::Exact,
            Domain.Tuples.front().front() + (Cached ? CacheBias : 0)};
  case FiniteValueStatus::TooManyValues:
    if (CompletedDomain)
      *CompletedDomain = Domain;
    return {FrameOffsetStatus::NonUnique};
  case FiniteValueStatus::Invalid:
    return {FrameOffsetStatus::Invalid};
  case FiniteValueStatus::Unknown:
  case FiniteValueStatus::QueryBudgetExceeded:
    if (!Cached && Normalization && Normalization->MaxWork)
      if (const auto Normalized = normalizeFrameExpression(
              Ctx, Value, MaxSymbolicNodes, *Normalization)) {
        // Algebra alone supplies no feasibility witness. An already completed
        // empty projection under the same predicate supplies the missing
        // evidence, without another solver query or a new memory premise.
        if (const auto Offset = frameRelativeOffset(Ctx, *Normalized, Root)) {
          if (Cache)
            if (auto Feasible = Cache->lookup(Ctx, Predicate, {}, 1);
                Feasible && Feasible->Status == FiniteValueStatus::Complete) {
              for (auto &Tuple : Feasible->Tuples)
                Tuple = {*Offset};
              RememberDomain(*Feasible);
              if (Feasible->Tuples.empty())
                return {FrameOffsetStatus::Infeasible};
            }
          return {FrameOffsetStatus::Exact, *Offset};
        }
        // An exact derivation may still need the ordinary predicate-dependent
        // proof. Retry once with a fresh encoding and the original limits and
        // cumulative query count. Do not normalize again or reset WorkUsed.
        // Exact normalization preserves every projected value under the same
        // predicate. A completed retry can therefore populate the original
        // query's cache, with its original constant bias. A structural offset
        // or incomplete retry supplies no finite-domain evidence to store.
        if (*Normalized != Value) {
          FiniteValues RetriedDomain;
          const auto Retried =
              proveFrameOffsetImpl(Ctx, Predicate, *Normalized, Root, Settings,
                                   MaxQueries, MaxSymbolicNodes, Queries,
                                   nullptr, nullptr, nullptr, &RetriedDomain);
          RememberDomain(RetriedDomain);
          return Retried;
        }
      }
    return {FrameOffsetStatus::BudgetExceeded};
  }
  llvm_unreachable("invalid finite value status");
}

FrameOffset proveFrameOffset(SymContext &Ctx, SymRef Predicate, SymRef Value,
                             SymRef Root, solver::SolverOptions Settings,
                             uint64_t MaxQueries, uint64_t MaxSymbolicNodes,
                             uint64_t &Queries, FiniteQueryCache *Cache,
                             FrameOffsetNormalization *Normalization) {
  return proveFrameOffsetImpl(Ctx, Predicate, Value, Root, Settings, MaxQueries,
                              MaxSymbolicNodes, Queries, Cache, nullptr,
                              Normalization);
}

FrameOffset proveFrameOffset(FiniteDomainEncoding &Encoding, SymRef Predicate,
                             SymRef Value, SymRef Root, uint64_t MaxQueries,
                             uint64_t MaxSymbolicNodes, uint64_t &Queries,
                             FiniteQueryCache *Cache,
                             FrameOffsetNormalization *Normalization) {
  return proveFrameOffsetImpl(Encoding.context(), Predicate, Value, Root,
                              Encoding.settings(), MaxQueries, MaxSymbolicNodes,
                              Queries, Cache, &Encoding, Normalization);
}

FrameOffset proveFrameOffset(SymContext &Ctx, SymRef Predicate, SymRef Value,
                             SymRef Root, const SpecializationOptions &Options,
                             uint64_t &Queries, FiniteQueryCache *Cache,
                             FrameOffsetNormalization *Normalization) {
  solver::SolverOptions Settings;
  Settings.Blast.MaxGates = Options.MaxSolverGates;
  Settings.Sat.MaxConflicts = Options.MaxSolverConflicts;
  Settings.Sat.MaxPropagations = Options.MaxSolverPropagations;
  Settings.Sat.MaxWatchVisits = Options.MaxSolverWatchVisits;
  return proveFrameOffset(Ctx, Predicate, Value, Root, Settings,
                          Options.MaxSolverQueries, Options.MaxSymbolicNodes,
                          Queries, Cache, Normalization);
}

} // namespace neverd::analysis::detail
