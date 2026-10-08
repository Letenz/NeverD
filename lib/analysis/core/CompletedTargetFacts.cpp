//===- CompletedTargetFacts.cpp - Facts from complete target enumeration
//===========//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "CompletedTargetFacts.h"

#include <algorithm>
#include <limits>
#include <vector>
namespace neverd::analysis::detail {
namespace {
enum class AdditiveReductionStatus { Unavailable, Reduced, BudgetExceeded };
struct AdditiveReduction {
  AdditiveReductionStatus Status = AdditiveReductionStatus::Unavailable;
  uint64_t Constant = 0;
  std::vector<symbolic::SymRef> Residual;
};

inline AdditiveReduction partitionCompletedAdd(const symbolic::SymContext &Ctx,
                                               symbolic::SymRef Target,
                                               symbolic::SymRef Known,
                                               uint64_t Constant,
                                               uint64_t &Remaining) {
  using namespace symbolic;
  const auto Charge = [&] { return Remaining ? (--Remaining, true) : false; };
  const auto Budget = [] {
    return AdditiveReduction{AdditiveReductionStatus::BudgetExceeded, 0, {}};
  };
  const auto Valid = [&](SymRef R) {
    return R && R.index() < Ctx.numNodes() && Ctx.width(R);
  };
  if (!Charge())
    return Budget();
  if (!Valid(Target) || !Valid(Known) || Ctx.width(Target) > 64 ||
      Ctx.width(Known) != Ctx.width(Target) || Ctx.op(Target) != SymOp::Add ||
      Ctx.op(Known) != SymOp::Add)
    return {};
  const unsigned Width = Ctx.width(Target);
  if (Width < 64 && (Constant >> Width))
    return {};
  llvm::APInt Offset(Width, Constant);
  std::vector<SymRef> Residual;
  for (auto Part : Ctx.operands(Target)) {
    if (!Charge())
      return Budget();
    if (!Valid(Part) || Ctx.width(Part) != Width)
      return {};
    if (auto K = Ctx.asConst(Part))
      Offset += *K;
    else
      Residual.push_back(Part);
  }
  uint64_t Removed = 0;
  for (auto Part : Ctx.operands(Known)) {
    if (!Charge())
      return Budget();
    if (!Valid(Part) || Ctx.width(Part) != Width)
      return {};
    if (auto K = Ctx.asConst(Part)) {
      Offset -= *K;
      continue;
    }
    bool Found = false;
    for (auto &Candidate : Residual) {
      if (!Charge())
        return Budget();
      if (Candidate == Part) {
        Candidate = {};
        Found = true;
        ++Removed;
        break;
      }
    }
    if (!Found)
      return {};
  }
  if (Removed < 2)
    return {};
  std::vector<SymRef> Kept;
  for (auto Part : Residual) {
    if (!Charge())
      return Budget();
    if (Part)
      Kept.push_back(Part);
  }
  return {AdditiveReductionStatus::Reduced, Offset.getZExtValue(),
          std::move(Kept)};
}

class TargetFactStorage {
  struct Fact {
    symbolic::SymRef Predicate, Value;
    uint64_t Constant;
  };
  static constexpr uint64_t WordsPerFact = (sizeof(Fact) + 7) / 8;
  const symbolic::SymContext &Context;
  const uint64_t Maximum;
  std::unique_ptr<Fact[]> Facts;
  uint64_t Count = 0, Capacity = 0;

  bool valid(symbolic::SymRef R) const {
    return R && R.index() < Context.numNodes() && Context.width(R);
  }

public:
  using Answer = CompletedTargetFacts::Answer;

private:
  Answer premise(symbolic::SymRef Domain, symbolic::SymRef Stored,
                 uint64_t &Remaining) const {
    using namespace symbolic;
    const auto Charge = [&]() {
      return Remaining ? (--Remaining, true) : false;
    };
    const auto Contains = [&](SymRef Literal) -> Answer {
      if (!Charge())
        return Answer::BudgetExceeded;
      if (Domain == Literal || Context.isConstOnes(Literal))
        return Answer::Proved;
      if (Context.op(Domain) != SymOp::And)
        return Answer::Unavailable;
      for (auto Part : Context.operands(Domain)) {
        if (!Charge())
          return Answer::BudgetExceeded;
        if (Part == Literal)
          return Answer::Proved;
      }
      return Answer::Unavailable;
    };
    if (Stored == Domain || Context.isConstOnes(Stored))
      return Answer::Proved;
    if (Context.op(Stored) != SymOp::And)
      return Contains(Stored);
    for (auto Part : Context.operands(Stored)) {
      const auto A = Contains(Part);
      if (A != Answer::Proved)
        return A;
    }
    return Answer::Proved;
  }

public:
  TargetFactStorage(const symbolic::SymContext &Ctx, uint64_t MaxWords)
      : Context(Ctx),
        Maximum(std::min(
            MaxWords / WordsPerFact,
            uint64_t(std::numeric_limits<size_t>::max() / sizeof(Fact)))) {}
  TargetFactStorage(const TargetFactStorage &) = delete;
  TargetFactStorage &operator=(const TargetFactStorage &) = delete;

  // Result must be the completed enumeration of this exact predicate/value.
  // The production caller inserts only after its final blocking UNSAT.
  bool store(const symbolic::SymContext &Ctx, symbolic::SymRef Predicate,
             symbolic::SymRef Value, const FiniteValues &Result) {
    if (&Ctx != &Context || !valid(Predicate) ||
        Context.width(Predicate) != 1 || !valid(Value) ||
        Context.width(Value) > 64 ||
        Result.Status != FiniteValueStatus::Complete ||
        Result.Tuples.size() != 1 || Result.Tuples.front().size() != 1 ||
        Count == Maximum)
      return false;
    const uint64_t Constant = Result.Tuples.front().front();
    const auto Width = Context.width(Value);
    if (Width < 64 && (Constant >> Width))
      return false;
    if (Count == Capacity) {
      const uint64_t Next = std::min(Maximum, Capacity ? 2 * Capacity : 1);
      auto New = std::make_unique<Fact[]>(Next);
      if (Count)
        std::copy_n(Facts.get(), Count, New.get());
      Facts = std::move(New);
      Capacity = Next;
    }
    Facts[Count++] = {Predicate, Value, Constant};
    return true;
  }

  // Every inspected fact and conjunction comparison consumes shared work.
  // Only an exact atom or a complete conjunction subset establishes a premise.
  Answer proves(const symbolic::SymContext &Ctx, symbolic::SymRef Domain,
                symbolic::SymRef Equality, uint64_t &Remaining) const {
    using namespace symbolic;
    const auto Charge = [&]() {
      return Remaining ? (--Remaining, true) : false;
    };
    if (!Charge())
      return Answer::BudgetExceeded;
    if (&Ctx != &Context || !valid(Domain) || Context.width(Domain) != 1 ||
        !valid(Equality) || Context.width(Equality) != 1)
      return Answer::Unavailable;
    if (Context.isConstOnes(Equality))
      return Answer::Proved;
    if (Context.op(Equality) != SymOp::Eq || Context.numOperands(Equality) != 2)
      return Answer::Unavailable;
    auto Value = Context.operand(Equality, 0);
    auto Constant = Context.operand(Equality, 1);
    if (!valid(Value) || !valid(Constant))
      return Answer::Unavailable;
    if (Context.isConst(Value))
      std::swap(Value, Constant);
    if (!valid(Value) || !valid(Constant) || !Context.isConst(Constant) ||
        Context.width(Value) > 64 ||
        Context.width(Constant) != Context.width(Value))
      return Answer::Unavailable;
    const auto Expected = Context.constValue(Constant).getZExtValue();
    for (uint64_t I = Count; I != 0;) {
      if (!Charge())
        return Answer::BudgetExceeded;
      const auto &F = Facts[--I];
      if (F.Value != Value || F.Constant != Expected)
        continue;
      const auto A = premise(Domain, F.Predicate, Remaining);
      if (A != Answer::Unavailable)
        return A;
    }
    return Answer::Unavailable;
  }

  struct Reduction {
    AdditiveReduction Plan;
    symbolic::SymRef Known;
  };
  Reduction reduce(const symbolic::SymContext &Ctx, symbolic::SymRef Domain,
                   symbolic::SymRef Target, uint64_t &Remaining) const {
    const auto Budget = [] {
      return Reduction{{AdditiveReductionStatus::BudgetExceeded, 0, {}}, {}};
    };
    const auto Charge = [&] { return Remaining ? (--Remaining, true) : false; };
    if (!Charge())
      return Budget();
    if (&Ctx != &Context || !valid(Domain) || Context.width(Domain) != 1 ||
        !valid(Target) || Context.width(Target) > 64 ||
        Context.op(Target) != symbolic::SymOp::Add)
      return {};
    for (uint64_t I = Count; I != 0;) {
      if (!Charge())
        return Budget();
      const auto &F = Facts[--I];
      if (Context.width(F.Value) != Context.width(Target) ||
          Context.op(F.Value) != symbolic::SymOp::Add)
        continue;
      const auto P = premise(Domain, F.Predicate, Remaining);
      if (P == Answer::BudgetExceeded)
        return Budget();
      if (P != Answer::Proved)
        continue;
      auto Plan = partitionCompletedAdd(Context, Target, F.Value, F.Constant,
                                        Remaining);
      if (Plan.Status == AdditiveReductionStatus::BudgetExceeded)
        return Budget();
      if (Plan.Status == AdditiveReductionStatus::Reduced)
        return {std::move(Plan), F.Value};
    }
    return {};
  }

  uint64_t size() const { return Count; }
  uint64_t allocatedWords() const { return Capacity * WordsPerFact; }
};
} // namespace

class CompletedTargetFacts::Impl {
public:
  symbolic::SymContext &Context;
  TargetFactStorage Facts;
  Impl(symbolic::SymContext &Ctx, uint64_t MaxWords)
      : Context(Ctx), Facts(Ctx, MaxWords) {}
};
CompletedTargetFacts::CompletedTargetFacts(symbolic::SymContext &Ctx,
                                           uint64_t MaxWords)
    : State(std::make_unique<Impl>(Ctx, MaxWords)) {}
CompletedTargetFacts::~CompletedTargetFacts() = default;
uint64_t CompletedTargetFacts::size() const { return State->Facts.size(); }
uint64_t CompletedTargetFacts::allocatedWords() const {
  return State->Facts.allocatedWords();
}
CompletedTargetFacts::Answer
CompletedTargetFacts::proves(const symbolic::SymContext &Ctx,
                             symbolic::SymRef Domain, symbolic::SymRef Equality,
                             uint64_t &Remaining) const {
  return State->Facts.proves(Ctx, Domain, Equality, Remaining);
}
FiniteValues CompletedTargetFacts::enumerate(
    FiniteDomainEncoding &Encoding, symbolic::SymRef Predicate,
    symbolic::SymRef Target, uint32_t Limit, uint64_t MaxQueries,
    uint64_t MaxSymbolicNodes, uint64_t &Queries) {
  auto &Ctx = State->Context;
  if (&Encoding.context() != &Ctx)
    return {FiniteValueStatus::Invalid, {}};
  auto Error = solver::BlastError::None;
  auto Values =
      enumerateFiniteValues(Encoding, Predicate, {Target}, Limit, MaxQueries,
                            MaxSymbolicNodes, Queries, Error);
  if (Values.Status == FiniteValueStatus::Unknown &&
      Error == solver::BlastError::TooManyGates && Queries < MaxQueries &&
      Ctx.numNodes() <= MaxSymbolicNodes) {
    uint64_t Remaining = MaxSymbolicNodes;
    auto Reduction = State->Facts.reduce(Ctx, Predicate, Target, Remaining);
    if (Reduction.Plan.Status == AdditiveReductionStatus::Reduced) {
      auto Parts = std::move(Reduction.Plan.Residual);
      Parts.push_back(Ctx.mkConst(Ctx.width(Target), Reduction.Plan.Constant));
      const auto Reduced = Ctx.mkAdd(Parts);
      if (Ctx.numNodes() > MaxSymbolicNodes)
        return {FiniteValueStatus::Unknown, {}};
      Values = enumerateFiniteValues(Encoding, Predicate, {Reduced}, Limit,
                                     MaxQueries, MaxSymbolicNodes, Queries);
    }
  }
  // The reduction itself is a conditional identity under this same domain;
  // only a complete result, including its final blocking UNSAT, grants a fact.
  if (Values.Status == FiniteValueStatus::Complete)
    State->Facts.store(Ctx, Predicate, Target, Values);
  return Values;
}
} // namespace neverd::analysis::detail
