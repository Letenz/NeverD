//===- DomainCoverageTests.cpp - Complete factor and partition proofs
//===========//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/analysis/core/DomainCoverage.h"
#include "gtest/gtest.h"

#include "neverd/solver/BitVectorSolver.h"

#include <stdexcept>
using namespace neverd;
using namespace neverd::symbolic;
using namespace neverd::analysis::detail;
namespace {
struct Fixture {
  SymContext C;
  SymRef X = C.mkVar("x", 2), Y = C.mkVar("y", 2), Z = C.mkVar("z", 2);
  SymRef Domain, Coverage;
  uint64_t Queries = 0, MaxQueries = 100, MaxNodes = 10000, RefuseAt = 0;
  solver::SatResult Refusal = solver::SatResult::Unknown;
  unsigned Mask, Bad;
  Fixture(unsigned M = 63, unsigned B = 6) : Mask(M), Bad(B) {
    Domain = C.mkAnd({C.mkUlt(X, C.mkConst(2, 2)), C.mkUlt(Y, C.mkConst(2, 3)),
                      C.mkEq(Z, C.mkAdd(X, Y))});
    std::vector<SymRef> Arms;
    for (unsigned I = 0; I != 6; ++I) {
      if (!(Mask & (1U << I)))
        continue;
      unsigned A = I / 3, B = I % 3, Sum = (A + B + (I == Bad)) & 3;
      Arms.push_back(
          C.mkAnd({Domain, C.mkEq(X, C.mkConst(2, A)),
                   C.mkEq(Y, C.mkConst(2, B)), C.mkEq(Z, C.mkConst(2, Sum))}));
    }
    Coverage = Arms.empty() ? C.mkFalse() : C.mkOr(Arms);
  }
  bool truth() const {
    for (unsigned A = 0; A != 4; ++A)
      for (unsigned B = 0; B != 4; ++B)
        for (unsigned V = 0; V != 4; ++V) {
          if (!(A < 2 && B < 3 && V == ((A + B) & 3)))
            continue;
          bool Covered = false;
          for (unsigned I = 0; I != 6; ++I)
            Covered |= (Mask & (1U << I)) && A == I / 3 && B == I % 3 &&
                       V == ((I / 3 + I % 3 + (I == Bad)) & 3);
          if (!Covered)
            return false;
        }
    return true;
  }
  void nodes() {
    if (C.numNodes() > MaxNodes)
      throw std::runtime_error("node budget");
  }
  solver::SatResult prove(SymRef D, SymRef Goal) {
    nodes();
    if (Queries >= MaxQueries)
      return solver::SatResult::Unknown;
    ++Queries;
    if (Queries == RefuseAt)
      return Refusal;
    auto Q = C.mkAnd(D, C.mkNot(Goal));
    nodes();
    solver::SolverOptions Options;
    Options.BuildModel = false;
    solver::BitVectorSolver S(C, Options);
    S.assertTrue(D);
    S.assertTrue(Q);
    return S.check();
  }
  PartitionedCoverageResult run(uint64_t Work = 1000000) {
    return provePartitionedCoverage(
        C, Domain, Coverage, Work,
        [&](SymRef D, SymRef Goal) { return prove(D, Goal); },
        [&] { nodes(); });
  }
};
TEST(PartitionedCoverage, ExhaustiveIndependentInputAndCaseMatrix) {
  for (unsigned Mask = 0; Mask != 64; ++Mask)
    for (unsigned Bad : {0U, 3U, 5U, 6U}) {
      SCOPED_TRACE(Mask);
      SCOPED_TRACE(Bad);
      Fixture F(Mask, Bad);
      auto Result = F.run();
      EXPECT_EQ(Result.Proved, F.truth());
    }
}
TEST(PartitionedCoverage, CompleteMultiLevelCoverProvesEveryGroup) {
  Fixture F;
  auto R = F.run();
  ASSERT_TRUE(R.Proved);
  EXPECT_EQ(R.Splits, 3U);
  EXPECT_EQ(R.Leaves, 6U);
  EXPECT_EQ(R.Obligations, 9U);
  EXPECT_EQ(F.Queries, 9U);
  EXPECT_EQ(R.MaximumDepth, 2U);
}
TEST(PartitionedCoverage, MissingValueCannotBeAssumedExhaustive) {
  Fixture F(7);
  auto R = F.run();
  EXPECT_FALSE(R.Proved);
  EXPECT_FALSE(F.truth());
  Fixture G(31);
  EXPECT_FALSE(G.run().Proved);
  EXPECT_FALSE(G.truth());
}
TEST(PartitionedCoverage, LastBadLeafCannotReuseEarlierSuccess) {
  Fixture F(63, 5);
  auto R = F.run();
  EXPECT_FALSE(R.Proved);
  EXPECT_GT(R.Leaves, 1U);
  EXPECT_FALSE(F.truth());
}
TEST(PartitionedCoverage, ExactAndShortGlobalQueryAllowances) {
  Fixture Full;
  auto R = Full.run();
  ASSERT_TRUE(R.Proved);
  for (uint64_t Q = 0; Q <= Full.Queries; ++Q) {
    SCOPED_TRACE(Q);
    Fixture F;
    F.MaxQueries = Q;
    auto T = F.run();
    EXPECT_EQ(T.Proved, Q == Full.Queries);
    EXPECT_LE(F.Queries, Q);
  }
}
TEST(PartitionedCoverage, EveryUnresolvedObligationRefusesTheWholeResult) {
  Fixture Full;
  ASSERT_TRUE(Full.run().Proved);
  for (auto Answer : {solver::SatResult::Unknown, solver::SatResult::Invalid})
    for (uint64_t Q = 1; Q <= Full.Queries; ++Q) {
      SCOPED_TRACE(static_cast<unsigned>(Answer));
      SCOPED_TRACE(Q);
      Fixture F;
      F.RefuseAt = Q;
      F.Refusal = Answer;
      EXPECT_FALSE(F.run().Proved);
      EXPECT_EQ(F.Queries, Q);
    }
}
TEST(PartitionedCoverage, ExactAndShortStructuralWorkAllowances) {
  Fixture Full;
  auto R = Full.run();
  ASSERT_TRUE(R.Proved);
  ASSERT_GT(R.Words, 1U);
  Fixture Exact;
  EXPECT_TRUE(Exact.run(R.Words).Proved);
  Fixture Short;
  EXPECT_FALSE(Short.run(R.Words - 1).Proved);
  Fixture Empty;
  EXPECT_FALSE(Empty.run(0).Proved);
  EXPECT_EQ(Empty.Queries, 0U);
}
TEST(PartitionedCoverage, NodeCeilingAppliesToConstructedQuestions) {
  Fixture Full;
  ASSERT_TRUE(Full.run().Proved);
  const auto End = Full.C.numNodes();
  Fixture Exact;
  Exact.MaxNodes = End;
  EXPECT_TRUE(Exact.run().Proved);
  Fixture Short;
  Short.MaxNodes = End - 1;
  EXPECT_THROW(Short.run(), std::runtime_error);
}
TEST(PartitionedCoverage, CompleteQuestionRetainsOriginalDomain) {
  Fixture F;
  F.Domain = F.C.mkTrue();
  EXPECT_FALSE(F.run().Proved);
  Fixture Empty;
  Empty.Domain = Empty.C.mkFalse();
  EXPECT_TRUE(Empty.run().Proved);
  Fixture All;
  All.Coverage = All.C.mkTrue();
  EXPECT_TRUE(All.run().Proved);
}
TEST(PartitionedCoverage, InvalidRootsNeverReachTheProofCallback) {
  SymContext C;
  unsigned Calls = 0;
  for (auto R : {SymRef{}, SymRef(0xfffffffe), C.mkVar("wide", 8)}) {
    auto P = provePartitionedCoverage(
        C, R, C.mkTrue(), 100,
        [&](SymRef, SymRef) {
          ++Calls;
          return solver::SatResult::Unsat;
        },
        [] {});
    EXPECT_FALSE(P.Proved);
    auto Q = provePartitionedCoverage(
        C, C.mkTrue(), R, 100,
        [&](SymRef, SymRef) {
          ++Calls;
          return solver::SatResult::Unsat;
        },
        [] {});
    EXPECT_FALSE(Q.Proved);
  }
  EXPECT_EQ(Calls, 0U);
}
TEST(PartitionedCoverage, MalformedEqualityOperandsNeverReachProof) {
  SymContext C;
  auto Wide = C.mkVar("wrong-width", 8);
  auto X = C.mkVar("target", 2);
  auto E = C.mkEq(X, C.mkConst(2, 0));
  auto Coverage = C.mkOr(E, C.mkEq(X, C.mkConst(2, 1)));
  auto Domain = C.mkTrue();
  unsigned Calls = 0;
  auto &Operand = const_cast<SymRef &>(C.operands(E)[0]);
  const auto Original = Operand;
  for (auto Invalid : {SymRef{}, SymRef(0xfffffffe), E, Wide}) {
    SCOPED_TRACE(Invalid.index());
    Operand = Invalid;
    auto Result = provePartitionedCoverage(
        C, Domain, Coverage, 10000,
        [&](SymRef, SymRef) {
          ++Calls;
          return solver::SatResult::Unsat;
        },
        [] {});
    Operand = Original;
    EXPECT_FALSE(Result.Proved);
  }
  EXPECT_EQ(Calls, 0U);
}
TEST(PartitionedCoverage, LeafCallbackCannotExceedNodeCeiling) {
  SymContext C;
  const auto Domain = C.mkTrue();
  const auto Ceiling = C.numNodes();
  unsigned Calls = 0;
  EXPECT_THROW(provePartitionedCoverage(
                   C, Domain, Domain, 1000,
                   [&](SymRef, SymRef) {
                     ++Calls;
                     C.mkFreshVar(1, "callback-growth");
                     return solver::SatResult::Unsat;
                   },
                   [&] {
                     if (C.numNodes() > Ceiling)
                       throw std::runtime_error("node budget");
                   }),
               std::runtime_error);
  EXPECT_EQ(Calls, 1U);
}
TEST(PartitionedCoverage, PartitionCallbackCannotExceedNodeCeiling) {
  for (auto Answer : {solver::SatResult::Unsat, solver::SatResult::Unknown,
                      solver::SatResult::Invalid}) {
    SCOPED_TRACE(static_cast<unsigned>(Answer));
    SymContext C;
    auto X = C.mkVar("target", 2);
    auto Domain = C.mkUlt(X, C.mkConst(2, 2));
    auto Coverage =
        C.mkOr(C.mkEq(X, C.mkConst(2, 0)), C.mkEq(X, C.mkConst(2, 1)));
    // Admit the selector constants so the callback itself crosses the ceiling.
    C.mkFalse();
    C.mkTrue();
    const auto Ceiling = C.numNodes();
    unsigned Calls = 0;
    EXPECT_THROW(provePartitionedCoverage(
                     C, Domain, Coverage, 10000,
                     [&](SymRef, SymRef) {
                       ++Calls;
                       C.mkFreshVar(1, "callback-growth");
                       return Answer;
                     },
                     [&] {
                       if (C.numNodes() > Ceiling)
                         throw std::runtime_error("node budget");
                     }),
                 std::runtime_error);
    EXPECT_EQ(Calls, 1U);
  }
}
TEST(PartitionedCoverage, UnrelatedResidualGuardIsNeverDropped) {
  Fixture F;
  auto Flag = F.C.mkVar("independent", 1);
  std::vector<SymRef> Arms(F.C.operands(F.Coverage).begin(),
                           F.C.operands(F.Coverage).end());
  Arms.back() = F.C.mkAnd(Arms.back(), Flag);
  F.Coverage = F.C.mkOr(Arms);
  EXPECT_FALSE(F.run().Proved);
}
TEST(PartitionedCoverage, ResourceBoundsRejectLargeUninspectedCovers) {
  SymContext C;
  std::vector<SymRef> Arms;
  for (unsigned I = 0; I != 9; ++I)
    Arms.push_back(C.mkVar("arm" + std::to_string(I), 1));
  unsigned Calls = 0;
  auto Result = provePartitionedCoverage(
      C, C.mkTrue(), C.mkOr(Arms), 100000,
      [&](SymRef, SymRef) {
        ++Calls;
        return solver::SatResult::Unsat;
      },
      [] {});
  EXPECT_FALSE(Result.Proved);
  EXPECT_EQ(Calls, 0U);
  auto LargeArm = C.mkAnd(Arms);
  for (unsigned I = 9; I != 65; ++I)
    LargeArm = C.mkAnd(LargeArm, C.mkVar("arm" + std::to_string(I), 1));
  Result = provePartitionedCoverage(
      C, C.mkTrue(), LargeArm, 100000,
      [&](SymRef, SymRef) {
        ++Calls;
        return solver::SatResult::Unsat;
      },
      [] {});
  EXPECT_FALSE(Result.Proved);
  EXPECT_EQ(Calls, 0U);
}
} // namespace

namespace {
TEST(DomainCoverage, EveryUnprovedFactorRemainsInTheFinalQuery) {
  for (bool Bad : {false, true}) {
    SymContext C;
    auto X = C.mkVar("x", 3), Y = C.mkVar("y", 3);
    auto B = C.mkVar("b", 1), A = C.mkVar("a", 1), D = C.mkEq(X, Y);
    auto Left = C.mkAnd(D, B), Right = C.mkAnd(D, C.mkNot(B));
    if (Bad) {
      Left = C.mkAnd(Left, A);
      Right = C.mkAnd(Right, A);
    }
    auto Cover = C.mkOr(Left, Right), Question = C.mkAnd(D, C.mkNot(Cover));
    unsigned Queries = 0;
    const auto Check = [&](SymRef Q) {
      ++Queries;
      return solver::checkSat(C, Q);
    };
    const auto R = proveCoverageFromDomainFacts(C, Question, D, Cover, 10000,
                                                Check, [] {});
    EXPECT_EQ(R.Proved, !Bad);
    EXPECT_EQ(R.FreshQueries, Queries);
    EXPECT_GT(R.Removed, 0U);
    // The oracle is independent: under x==y, exactly one Boolean branch holds;
    // adding a=false to both arms leaves that entire input slice uncovered.
    unsigned Missing = 0;
    for (unsigned Xv = 0; Xv < 8; ++Xv)
      for (unsigned Yv = 0; Yv < 8; ++Yv)
        for (bool Bv : {false, true})
          for (bool Av : {false, true})
            Missing += (Xv == Yv) && !(((Xv == Yv) && Bv && (!Bad || Av)) ||
                                       ((Xv == Yv) && !Bv && (!Bad || Av)));
    EXPECT_EQ(Missing == 0, R.Proved);
    EXPECT_FALSE(proveCoverageFromDomainFacts(C, C.mkNot(Question), D, Cover,
                                              10000, Check, [] {})
                     .Proved);
    EXPECT_FALSE(
        proveCoverageFromDomainFacts(C, Question, D, Cover, 0, Check, [] {
        }).Proved);
    if (R.Proved) {
      EXPECT_TRUE(proveCoverageFromDomainFacts(C, Question, D, Cover, R.Words,
                                               Check, [] {})
                      .Proved);
      EXPECT_FALSE(proveCoverageFromDomainFacts(C, Question, D, Cover,
                                                R.Words - 1, Check, [] {})
                       .Proved);
    }
  }
}
TEST(DomainCoverage, CallbackAllocationIsCheckedEvenOnRefusal) {
  for (auto Answer : {solver::SatResult::Unsat, solver::SatResult::Unknown,
                      solver::SatResult::Invalid}) {
    SymContext C;
    auto D = C.mkVar("d", 1), X = C.mkVar("x", 1), Y = C.mkVar("y", 1);
    auto Cover = C.mkOr(C.mkAnd(D, X), C.mkAnd(D, Y));
    auto Q = C.mkAnd(D, C.mkNot(Cover));
    unsigned Calls = 0;
    const auto Ceiling = C.numNodes() + 2;
    EXPECT_THROW(proveCoverageFromDomainFacts(
                     C, Q, D, Cover, 10000,
                     [&](SymRef) {
                       ++Calls;
                       for (unsigned I = 0; I < 4; ++I)
                         C.mkFreshVar(1);
                       return Answer;
                     },
                     [&] {
                       if (C.numNodes() > Ceiling)
                         throw std::runtime_error("node budget");
                     }),
                 std::runtime_error);
    EXPECT_EQ(Calls, 1U);
  }
}

struct BooleanCoverageRun {
  PartitionedCoverageResult Proof;
  uint64_t Queries = 0;
};
BooleanCoverageRun
booleanCoverage(unsigned Mask = 15, unsigned Bad = 4, uint64_t MaxQueries = 100,
                uint64_t RefuseAt = 0,
                solver::SatResult Refusal = solver::SatResult::Unknown,
                uint64_t Work = 1000000) {
  SymContext C;
  auto D = C.mkVar("domain", 1), Extra = C.mkVar("unrelated", 1);
  std::vector<SymRef> Decisions;
  for (unsigned I = 0; I != 3; ++I)
    Decisions.push_back(C.mkVar("choice" + std::to_string(I), 1));
  std::vector<SymRef> Arms;
  for (unsigned I = 0; I != 4; ++I) {
    if (!(Mask & (1U << I)))
      continue;
    std::vector<SymRef> Factors{I == Bad ? Extra : D};
    for (unsigned J = 0; J < I; ++J)
      Factors.push_back(C.mkNot(Decisions[J]));
    if (I < 3)
      Factors.push_back(Decisions[I]);
    Arms.push_back(C.mkAnd(Factors));
  }
  auto Cover = Arms.empty() ? C.mkFalse() : C.mkOr(Arms);
  BooleanCoverageRun R;
  R.Proof = provePartitionedCoverage(
      C, D, Cover, Work,
      [&](SymRef Domain, SymRef Goal) {
        if (R.Queries == MaxQueries)
          return solver::SatResult::Unknown;
        if (++R.Queries == RefuseAt)
          return Refusal;
        solver::BitVectorSolver S(C);
        S.assertTrue(Domain);
        S.assertTrue(C.mkAnd(Domain, C.mkNot(Goal)));
        return S.check();
      },
      [] {});
  return R;
}
TEST(PartitionedCoverage, BooleanDecisionChainsRetainEveryInputAndArm) {
  for (unsigned Mask = 0; Mask != 16; ++Mask)
    for (unsigned Bad = 0; Bad != 5; ++Bad) {
      SCOPED_TRACE(Mask);
      SCOPED_TRACE(Bad);
      bool Expected = true;
      for (unsigned Bits = 0; Bits != 8; ++Bits)
        for (bool Extra : {false, true}) {
          unsigned Selected = (Bits & 1)   ? 0
                              : (Bits & 2) ? 1
                              : (Bits & 4) ? 2
                                           : 3;
          Expected &= (Mask & (1U << Selected)) && (Selected != Bad || Extra);
        }
      EXPECT_EQ(booleanCoverage(Mask, Bad).Proof.Proved, Expected);
    }
  const auto Full = booleanCoverage();
  ASSERT_TRUE(Full.Proof.Proved);
  EXPECT_EQ(Full.Proof.Splits, 3U);
  EXPECT_EQ(Full.Proof.Leaves, 4U);
  EXPECT_EQ(Full.Proof.MaximumDepth, 3U);
  EXPECT_EQ(Full.Queries, 7U);
}
TEST(PartitionedCoverage, BooleanChainsChargeEveryBranchAndWorkBoundary) {
  const auto Full = booleanCoverage();
  ASSERT_TRUE(Full.Proof.Proved);
  for (uint64_t Q = 0; Q <= Full.Queries; ++Q) {
    const auto R = booleanCoverage(15, 4, Q);
    EXPECT_EQ(R.Proof.Proved, Q == Full.Queries);
    EXPECT_LE(R.Queries, Q);
  }
  for (auto Refusal : {solver::SatResult::Unknown, solver::SatResult::Invalid})
    for (uint64_t Q = 1; Q <= Full.Queries; ++Q) {
      const auto R = booleanCoverage(15, 4, 100, Q, Refusal);
      EXPECT_FALSE(R.Proof.Proved);
      EXPECT_EQ(R.Queries, Q);
    }
  EXPECT_TRUE(booleanCoverage(15, 4, 100, 0, solver::SatResult::Unknown,
                              Full.Proof.Words)
                  .Proof.Proved);
  EXPECT_FALSE(booleanCoverage(15, 4, 100, 0, solver::SatResult::Unknown,
                               Full.Proof.Words - 1)
                   .Proof.Proved);
}
TEST(PartitionedCoverage, MalformedBooleanNegationsCannotSelectAPartition) {
  SymContext C;
  auto Wide = C.mkVar("wide", 8), B = C.mkVar("boolean", 1);
  auto Cover = C.mkNot(B);
  auto &Operand = const_cast<SymRef &>(C.operands(Cover)[0]);
  for (auto Bad : {SymRef{}, SymRef(0xfffffffe), Wide, Cover}) {
    Operand = Bad;
    unsigned Calls = 0;
    const auto R = provePartitionedCoverage(
        C, C.mkTrue(), Cover, 1000,
        [&](SymRef, SymRef) {
          ++Calls;
          return solver::SatResult::Unsat;
        },
        [] {});
    EXPECT_FALSE(R.Proved);
    EXPECT_EQ(Calls, 0U);
    Operand = B;
  }
}
} // namespace
