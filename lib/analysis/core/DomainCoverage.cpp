//===- DomainCoverage.cpp - Complete terminal coverage proofs ===========//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "DomainCoverage.h"

#include <algorithm>
#include <map>
#include <vector>
namespace neverd::analysis::detail {
// Check must solve its complete question with no additional assumptions.
// It charges the caller's query allowance before each search. ValidateNodes
// enforces that caller's unchanged context allocation bound, including after
// each expression construction. Neither callback may accept Unknown as proof.
DomainCoverageProof proveCoverageFromDomainFacts(
    symbolic::SymContext &Ctx, symbolic::SymRef Predicate,
    symbolic::SymRef Domain, symbolic::SymRef Coverage, uint64_t MaxWork,
    llvm::function_ref<solver::SatResult(symbolic::SymRef)> Check,
    llvm::function_ref<void()> ValidateNodes) {
  using namespace symbolic;
  DomainCoverageProof Proof;
  uint64_t Remaining = MaxWork;
  const auto Charge = [&] { return Remaining ? (--Remaining, true) : false; };
  const auto Finish = [&](bool Proved) {
    Proof.Proved = Proved;
    Proof.Words = MaxWork - Remaining;
    return Proof;
  };
  const auto Boolean = [&](SymRef R) {
    return R && R.index() < Ctx.numNodes() && Ctx.width(R) == 1;
  };
  if (!Charge() || !Boolean(Predicate) || !Boolean(Domain) ||
      !Boolean(Coverage))
    return Finish(false);
  if ((Ctx.op(Domain) == SymOp::And && Ctx.numOperands(Domain) > 64) ||
      (Ctx.op(Coverage) == SymOp::Or && Ctx.numOperands(Coverage) > 8))
    return Finish(false);
  ValidateNodes();
  const auto Expected = Ctx.mkAnd(Domain, Ctx.mkNot(Coverage));
  ValidateNodes();
  if (Expected != Predicate)
    return Finish(false);
  // Copy all operand lists before any subsequent context mutation.
  const auto Terms = [&](SymRef R, SymOp Op) {
    std::vector<SymRef> Out;
    if (Ctx.op(R) == Op)
      Out.assign(Ctx.operands(R).begin(), Ctx.operands(R).end());
    else
      Out.push_back(R);
    return Out;
  };
  const auto DomainTerms = Terms(Domain, SymOp::And);
  const auto Arms = Terms(Coverage, SymOp::Or);
  if (DomainTerms.size() > 64 || Arms.size() > 8)
    return Finish(false);
  for (auto Factor : DomainTerms)
    if (!Charge() || !Boolean(Factor))
      return Finish(false);
  std::vector<std::vector<SymRef>> Residuals;
  std::vector<SymRef> Unique;
  for (auto Arm : Arms) {
    if (!Charge() || !Boolean(Arm))
      return Finish(false);
    if (Ctx.op(Arm) == SymOp::And && Ctx.numOperands(Arm) > 64)
      return Finish(false);
    const auto Factors = Terms(Arm, SymOp::And);
    if (Factors.size() > 64)
      return Finish(false);
    std::vector<SymRef> Kept;
    for (auto Factor : Factors) {
      if (!Charge() || !Boolean(Factor))
        return Finish(false);
      bool Implied = Ctx.isConstOnes(Factor);
      for (auto Part : DomainTerms) {
        if (!Charge())
          return Finish(false);
        Implied |= Factor == Part;
      }
      if (Implied) {
        ++Proof.Removed;
        continue;
      }
      Kept.push_back(Factor);
      bool Seen = false;
      for (auto Previous : Unique) {
        if (!Charge())
          return Finish(false);
        Seen |= Factor == Previous;
      }
      if (!Seen) {
        if (Unique.size() == 64)
          return Finish(false);
        Unique.push_back(Factor);
      }
    }
    Residuals.push_back(std::move(Kept));
  }
  Proof.Guards = Unique.size();
  std::vector<SymRef> Proven;
  for (auto Guard : Unique) {
    if (!Charge())
      return Finish(false);
    ValidateNodes();
    const auto Question = Ctx.mkAnd(Domain, Ctx.mkNot(Guard));
    ValidateNodes();
    ++Proof.FreshQueries;
    const auto Answer = Check(Question);
    ValidateNodes();
    if (Answer == solver::SatResult::Unsat)
      Proven.push_back(Guard);
  }
  std::vector<SymRef> ReducedArms;
  for (const auto &Arm : Residuals) {
    std::vector<SymRef> Kept;
    for (auto Factor : Arm) {
      bool Implied = false;
      for (auto Known : Proven) {
        if (!Charge())
          return Finish(false);
        Implied |= Factor == Known;
      }
      if (Implied)
        ++Proof.Removed;
      else
        Kept.push_back(Factor);
    }
    ValidateNodes();
    ReducedArms.push_back(Kept.empty() ? Ctx.mkTrue() : Ctx.mkAnd(Kept));
    ValidateNodes();
  }
  // An unresolved factor always remains. D implies every erased factor, so
  // this whole reduced-coverage query is equivalent to the original within D.
  if (!Proof.Removed || !Charge())
    return Finish(false);
  const auto ReducedCoverage = Ctx.mkOr(ReducedArms);
  const auto FinalQuestion = Ctx.mkAnd(Domain, Ctx.mkNot(ReducedCoverage));
  ValidateNodes();
  ++Proof.FreshQueries;
  const auto Answer = Check(FinalQuestion);
  ValidateNodes();
  return Finish(Answer == solver::SatResult::Unsat);
}

// Prove(D, G) must discharge the COMPLETE D => G under the same cumulative
// node/query and solver limits. A group is selected only after proving all
// proposed values exhaustive under D; every nonempty group must then pass.
// These are fresh proof obligations, never an assumption that observed paths
// exhaust the entry domain. No completed prefix alone completes this result.
PartitionedCoverageResult provePartitionedCoverage(
    symbolic::SymContext &Ctx, symbolic::SymRef Domain,
    symbolic::SymRef Coverage, uint64_t MaxWork,
    llvm::function_ref<solver::SatResult(symbolic::SymRef, symbolic::SymRef)>
        Prove,
    llvm::function_ref<void()> ValidateNodes) {
  using namespace symbolic;
  PartitionedCoverageResult Result;
  uint64_t Remaining = MaxWork;
  const auto Charge = [&](uint64_t N = 1) {
    if (N > Remaining)
      return false;
    Remaining -= N;
    return true;
  };
  const auto Boolean = [&](SymRef R) {
    return R && R.index() < Ctx.numNodes() && Ctx.width(R) == 1;
  };
  const auto ValidEquality = [&](SymRef R) {
    if (Ctx.op(R) != SymOp::Eq)
      return true;
    if (Ctx.numOperands(R) != 2 || !Charge(2))
      return false;
    for (auto A : Ctx.operands(R))
      if (!A || A.index() >= R.index() || A.index() >= Ctx.numNodes() ||
          !Ctx.width(A))
        return false;
    return Ctx.width(Ctx.operand(R, 0)) == Ctx.width(Ctx.operand(R, 1));
  };
  using FactorList = std::vector<SymRef>;
  using Arms = std::vector<FactorList>;
  if (!Charge() || !Boolean(Domain) || !Boolean(Coverage))
    return Result;
  std::vector<SymRef> Source;
  if (Ctx.op(Coverage) == SymOp::Or) {
    if (Ctx.numOperands(Coverage) > 8 || !Charge(Ctx.numOperands(Coverage)))
      return Result;
    Source.assign(Ctx.operands(Coverage).begin(), Ctx.operands(Coverage).end());
  } else
    Source.push_back(Coverage);
  Arms Initial;
  for (auto Root : Source) {
    if (!Charge() || !Boolean(Root))
      return Result;
    FactorList Factors;
    if (Ctx.op(Root) == SymOp::And) {
      if (Ctx.numOperands(Root) > 64 || !Charge(Ctx.numOperands(Root)))
        return Result;
      Factors.assign(Ctx.operands(Root).begin(), Ctx.operands(Root).end());
    } else
      Factors.push_back(Root);
    for (auto Factor : Factors) {
      if (!Charge() || !Boolean(Factor) || !ValidEquality(Factor))
        return Result;
      if (Ctx.op(Factor) == SymOp::Not) {
        if (Ctx.numOperands(Factor) != 1 || !Charge())
          return Result;
        const auto Inner = Ctx.operand(Factor, 0);
        if (!Boolean(Inner) || Inner.index() >= Factor.index())
          return Result;
      }
    }
    Initial.push_back(std::move(Factors));
  }
  const auto Equality = [&](SymRef Factor, SymRef &Target, SymRef &Constant) {
    if (!Charge())
      return false;
    if (Ctx.op(Factor) != SymOp::Eq || Ctx.numOperands(Factor) != 2)
      return false;
    if (!ValidEquality(Factor))
      return false;
    Target = Ctx.operand(Factor, 0);
    Constant = Ctx.operand(Factor, 1);
    if (Ctx.isConst(Target))
      std::swap(Target, Constant);
    return !Ctx.isConst(Target) && Ctx.isConst(Constant) &&
           Ctx.width(Target) > 0 && Ctx.width(Target) <= 64 &&
           Ctx.width(Target) == Ctx.width(Constant);
  };
  // A lowered decision chain may spell one edge as a predicate and the other
  // as its negation. Treat that exact Boolean identity as a two-value selector;
  // no omitted condition or merely similar expression supplies an edge.
  const auto Literal = [&](SymRef Factor, SymRef &Target, SymRef &Constant) {
    if (!Charge())
      return false;
    const bool Negated = Ctx.op(Factor) == SymOp::Not;
    if (Negated) {
      if (Ctx.numOperands(Factor) != 1 || !Charge())
        return false;
      Target = Ctx.operand(Factor, 0);
      if (!Boolean(Target) || Target.index() >= Factor.index())
        return false;
    } else
      Target = Factor;
    Constant = Negated ? Ctx.mkFalse() : Ctx.mkTrue();
    ValidateNodes();
    return !Ctx.isConst(Target);
  };
  const auto Whole = [&](SymRef D, const Arms &Current) {
    if (!Charge(1 + Current.size()))
      return false;
    std::vector<SymRef> Parts;
    for (const auto &Arm : Current) {
      if (!Charge(1 + Arm.size()))
        return false;
      Parts.push_back(Arm.empty() ? Ctx.mkTrue() : Ctx.mkAnd(Arm));
      ValidateNodes();
    }
    auto Goal = Ctx.mkOr(Parts);
    ValidateNodes();
    ++Result.Obligations;
    const auto Answer = Prove(D, Goal);
    ValidateNodes();
    return Answer == solver::SatResult::Unsat;
  };
  const auto Visit = [&](auto &&Self, SymRef D, const Arms &Current,
                         unsigned Depth) -> bool {
    Result.MaximumDepth = std::max(Result.MaximumDepth, Depth);
    if (!Charge() || Current.empty() || Current.size() > 8 || Depth >= 8)
      return false;
    if (Current.size() == 1) {
      ++Result.Leaves;
      return Whole(D, Current);
    }
    // Prefer the existing multi-value equalities before binary literals.
    std::vector<std::pair<bool, SymRef>> Candidates;
    for (auto Factor : Current.front()) {
      SymRef Target, Constant;
      if (Equality(Factor, Target, Constant))
        Candidates.push_back({false, Target});
      if (Literal(Factor, Target, Constant))
        Candidates.push_back({true, Target});
      if (!Remaining)
        return false;
    }
    std::sort(Candidates.begin(), Candidates.end());
    Candidates.erase(std::unique(Candidates.begin(), Candidates.end()),
                     Candidates.end());
    for (auto [BooleanSelector, Target] : Candidates) {
      if (!Charge())
        return false;
      std::map<SymRef, Arms> Groups;
      bool Complete = true;
      for (const auto &Arm : Current) {
        if (!Charge())
          return false;
        SymRef Value, Matched;
        for (auto Factor : Arm) {
          SymRef Other, Constant;
          const bool Matches = BooleanSelector
                                   ? Literal(Factor, Other, Constant)
                                   : Equality(Factor, Other, Constant);
          if (Matches && Other == Target) {
            if (Matched && Value != Constant) {
              Complete = false;
              break;
            }
            Value = Constant;
            Matched = Factor;
          }
          if (!Remaining)
            return false;
        }
        if (!Complete || !Matched) {
          Complete = false;
          break;
        }
        FactorList Rest;
        for (auto Factor : Arm) {
          if (!Charge())
            return false;
          if (Factor != Matched)
            Rest.push_back(Factor);
        }
        Groups[Value].push_back(std::move(Rest));
      }
      if (!Complete || Groups.size() < 2)
        continue;
      if (!Charge(1 + Groups.size()))
        return false;
      std::vector<SymRef> Values;
      for (const auto &[Value, Children] : Groups)
        Values.push_back(Ctx.mkEq(Target, Value));
      auto Exhaustive = Ctx.mkOr(Values);
      ValidateNodes();
      ++Result.Obligations;
      const auto Answer = Prove(D, Exhaustive);
      ValidateNodes();
      if (Answer != solver::SatResult::Unsat)
        return false;
      ++Result.Splits;
      for (const auto &[Value, Children] : Groups) {
        if (!Charge() || Children.size() >= Current.size())
          return false;
        auto ChildDomain = Ctx.mkAnd(D, Ctx.mkEq(Target, Value));
        ValidateNodes();
        if (!Self(Self, ChildDomain, Children, Depth + 1))
          return false;
      }
      return true;
    }
    ++Result.Leaves;
    return Whole(D, Current);
  };
  Result.Proved = Visit(Visit, Domain, Initial, 0);
  Result.Words = MaxWork - Remaining;
  return Result;
}
} // namespace neverd::analysis::detail
