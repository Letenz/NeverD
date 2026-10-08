//===- ConditionalImplicationTests.cpp - Complete conditional proofs ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../lib/analysis/core/ConditionalImplication.h"
#include "gtest/gtest.h"

#include <functional>
using namespace neverd;
using namespace neverd::symbolic;
using namespace neverd::solver;
namespace ci = neverd::analysis::conditional_implication;
namespace {
struct Inputs {
  SymContext C;
  SymRef X = C.mkVar("x", 3), Y = C.mkVar("y", 3), B = C.mkVar("b", 1),
         F = C.mkVar("f", 1);
};
using Build = std::function<std::pair<SymRef, SymRef>(Inputs &)>;
using Truth = std::function<bool(unsigned, unsigned, bool, bool)>;
struct Case {
  Build Expressions;
  Truth Domain, Goal;
};
TEST(ConditionalImplication, IndependentCompleteInputTruthTables) {
  const std::vector<Case> Cases{
      {[](auto &S) {
         return std::pair{S.C.mkEq(S.X, S.Y),
                          S.C.mkEq(S.C.mkMul(S.X, S.X), S.C.mkMul(S.Y, S.Y))};
       },
       [](auto X, auto Y, auto, auto) { return X == Y; },
       [](auto X, auto Y, auto, auto) {
         return ((X * X) & 7) == ((Y * Y) & 7);
       }},
      {[](auto &S) { return std::pair{S.C.mkTrue(), S.C.mkEq(S.X, S.Y)}; },
       [](auto, auto, auto, auto) { return true; },
       [](auto X, auto Y, auto, auto) { return X == Y; }},
      {[](auto &S) { return std::pair{S.C.mkOr(S.B, S.F), S.B}; },
       [](auto, auto, auto B, auto F) { return B || F; },
       [](auto, auto, auto B, auto) { return B; }},
      {[](auto &S) {
         return std::pair{S.C.mkNot(S.C.mkAnd(S.B, S.F)), S.C.mkNot(S.B)};
       },
       [](auto, auto, auto B, auto F) { return !(B && F); },
       [](auto, auto, auto B, auto) { return !B; }},
      {[](auto &S) {
         return std::pair{S.C.mkNot(S.C.mkOr(S.B, S.F)),
                          S.C.mkAnd(S.C.mkNot(S.B), S.C.mkNot(S.F))};
       },
       [](auto, auto, auto B, auto F) { return !(B || F); },
       [](auto, auto, auto B, auto F) { return !B && !F; }},
      {[](auto &S) {
         return std::pair{S.C.mkEq(S.X, S.Y), S.C.mkUlt(S.X, S.Y)};
       },
       [](auto X, auto Y, auto, auto) { return X == Y; },
       [](auto X, auto Y, auto, auto) { return X < Y; }},
      {[](auto &S) {
         return std::pair{S.C.mkEq(S.X, S.Y), S.C.mkSle(S.X, S.Y)};
       },
       [](auto X, auto Y, auto, auto) { return X == Y; },
       [](auto X, auto Y, auto, auto) {
         return (int(X ^ 4) - 4) <= (int(Y ^ 4) - 4);
       }},
      {[](auto &S) {
         return std::pair{S.C.mkEq(S.C.mkZExt(S.B, 3), S.X),
                          S.C.mkUle(S.X, S.C.mkConst(3, 1))};
       },
       [](auto X, auto, auto B, auto) { return X == unsigned(B); },
       [](auto X, auto, auto, auto) { return X <= 1; }},
      {[](auto &S) {
         return std::pair{S.C.mkEq(S.C.mkSExt(S.B, 3), S.C.mkConst(3, 7)), S.B};
       },
       [](auto, auto, auto B, auto) { return B; },
       [](auto, auto, auto B, auto) { return B; }},
      {[](auto &S) {
         return std::pair{S.C.mkEq(S.C.mkSExt(S.B, 3), S.C.mkConst(3, 1)), S.F};
       },
       [](auto, auto, auto, auto) { return false; },
       [](auto, auto, auto, auto F) { return F; }},
      {[](auto &S) {
         return std::pair{S.C.mkEq(S.C.mkAdd(S.X, S.C.mkConst(3, 1)), S.Y),
                          S.C.mkEq(S.X, S.C.mkAdd(S.Y, S.C.mkConst(3, 7)))};
       },
       [](auto X, auto Y, auto, auto) { return ((X + 1) & 7) == Y; },
       [](auto X, auto Y, auto, auto) { return X == ((Y + 7) & 7); }},
      {[](auto &S) {
         return std::pair{
             S.C.mkEq(S.X, S.Y),
             S.C.mkEq(S.C.mkExtract(S.X, 0, 1), S.C.mkExtract(S.Y, 1, 1))};
       },
       [](auto X, auto Y, auto, auto) { return X == Y; },
       [](auto X, auto Y, auto, auto) { return (X & 1) == ((Y >> 1) & 1); }},
      {[](auto &S) { return std::pair{S.C.mkXor(S.B, S.F), S.B}; },
       [](auto, auto, auto B, auto F) { return B != F; },
       [](auto, auto, auto B, auto) { return B; }},
      {[](auto &S) {
         return std::pair{S.C.mkIte(S.B, S.F, S.C.mkNot(S.F)), S.F};
       },
       [](auto, auto, auto B, auto F) { return B ? F : !F; },
       [](auto, auto, auto, auto F) { return F; }},
      {[](auto &S) { return std::pair{S.C.mkFalse(), S.C.mkFalse()}; },
       [](auto, auto, auto, auto) { return false; },
       [](auto, auto, auto, auto) { return false; }},
      {[](auto &S) {
         return std::pair{S.C.mkAnd(S.C.mkEq(S.X, S.C.mkConst(3, 2)),
                                    S.C.mkEq(S.X, S.C.mkConst(3, 5))),
                          S.F};
       },
       [](auto X, auto, auto, auto) { return X == 2 && X == 5; },
       [](auto, auto, auto, auto F) { return F; }}};
  unsigned Index = 0;
  for (const auto &Test : Cases) {
    SCOPED_TRACE(Index++);
    Inputs S;
    auto [D, G] = Test.Expressions(S);
    bool Expected = true;
    for (unsigned X = 0; X < 8; ++X)
      for (unsigned Y = 0; Y < 8; ++Y)
        for (bool B : {false, true})
          for (bool F : {false, true})
            if (Test.Domain(X, Y, B, F) && !Test.Goal(X, Y, B, F))
              Expected = false;
    ci::Cache Cache;
    uint64_t Q = 0;
    auto Result = Cache.proveFresh(S.C, D, G, {}, {}, 100, [&] {
      ++Q;
      return true;
    });
    EXPECT_EQ(Result.Proved, Expected);
    EXPECT_EQ(Result.Queries, Q);
    if (Expected)
      EXPECT_EQ(Result.ProvedTerms, Result.TotalTerms);
  }
}
TEST(ConditionalImplication, FreshVariablesAndNewGoalsRequireNewProofs) {
  SymContext C;
  auto X = C.mkVar("x", 3), Y = C.mkVar("y", 3);
  auto D = C.mkEq(C.mkAdd(X, C.mkConst(3, 1)), Y);
  auto G = C.mkEq(X, C.mkAdd(Y, C.mkConst(3, 7)));
  ci::Cache Cache;
  ASSERT_TRUE(
      Cache.proveFresh(C, D, G, {}, {}, 100, [] { return true; }).Proved);
  auto Z = C.mkFreshVar(3, "x");
  std::vector<
      std::pair<SymRef, std::function<bool(unsigned, unsigned, unsigned)>>>
      Goals{{G, [](auto X, auto Y, auto) { return X == ((Y + 7) & 7); }},
            {C.mkEq(X, Y), [](auto X, auto Y, auto) { return X == Y; }},
            {C.mkEq(Z, X), [](auto X, auto, auto Z) { return X == Z; }},
            {C.mkEq(Z, C.mkConst(3, 0)),
             [](auto, auto, auto Z) { return Z == 0; }},
            {C.mkEq(C.mkExtract(X, 0, 1), C.mkNot(C.mkExtract(Y, 0, 1))),
             [](auto X, auto Y, auto) { return (X & 1) != (Y & 1); }},
            {C.mkEq(C.mkExtract(X, 0, 1), C.mkExtract(Y, 1, 1)),
             [](auto X, auto Y, auto) { return (X & 1) == ((Y >> 1) & 1); }}};
  for (const auto &[Goal, Host] : Goals) {
    bool Expected = true;
    for (unsigned Xv = 0; Xv < 8; ++Xv)
      for (unsigned Yv = 0; Yv < 8; ++Yv)
        for (unsigned Zv = 0; Zv < 8; ++Zv)
          if (((Xv + 1) & 7) == Yv && !Host(Xv, Yv, Zv))
            Expected = false;
    unsigned Q = 0;
    const auto Result = Cache.proveCached(C, D, Goal, {}, {}, [&] {
      ++Q;
      return true;
    });
    EXPECT_TRUE(Result.ReusedPreparation);
    EXPECT_EQ(Result.Proved, Expected);
    EXPECT_EQ(Result.Queries, Q);
    EXPECT_GT(Q, 0U);
  }
}
TEST(ConditionalImplication,
     CacheIdentityIncludesContextDomainAndEveryPolicyField) {
  SymContext C, Other;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8), D = C.mkEq(X, Y);
  auto OX = Other.mkVar("x", 8), OY = Other.mkVar("y", 8),
       OD = Other.mkEq(OX, OY);
  ASSERT_EQ(D.index(), OD.index());
  ci::Cache Cache;
  const auto Seed = [&] {
    return Cache.proveFresh(C, D, D, {}, {}, 100, [] { return true; }).Proved;
  };
  ASSERT_TRUE(Seed());
  unsigned Q = 0;
  EXPECT_FALSE(Cache
                   .proveCached(Other, OD, OD, {}, {},
                                [&] {
                                  ++Q;
                                  return true;
                                })
                   .Proved);
  EXPECT_EQ(Q, 0U);
  ASSERT_TRUE(Seed());
  EXPECT_FALSE(Cache
                   .proveCached(C, C.mkTrue(), D, {}, {},
                                [&] {
                                  ++Q;
                                  return true;
                                })
                   .Proved);
  EXPECT_EQ(Q, 0U);
  using Change = std::function<void(SolverOptions &, ci::Limits &)>;
  const std::vector<Change> Changes{
      [](auto &O, auto &) { O.BuildModel = !O.BuildModel; },
      [](auto &O, auto &) { --O.Blast.MaxWidth; },
      [](auto &O, auto &) { --O.Blast.MaxGates; },
      [](auto &O, auto &) { O.Sat.VarDecay = 0.9; },
      [](auto &O, auto &) { O.Sat.ClauseDecay = 0.9; },
      [](auto &O, auto &) { ++O.Sat.RestartInterval; },
      [](auto &O, auto &) { O.Sat.LearnedFraction = 0.2; },
      [](auto &O, auto &) { O.Sat.LearnedGrowth = 1.2; },
      [](auto &O, auto &) { ++O.Sat.MaxConflicts; },
      [](auto &O, auto &) { ++O.Sat.MaxPropagations; },
      [](auto &O, auto &) { ++O.Sat.MaxWatchVisits; },
      [](auto &O, auto &) { O.Sat.MinimizeLearned = !O.Sat.MinimizeLearned; },
      [](auto &O, auto &) { O.Sat.PhaseSaving = !O.Sat.PhaseSaving; },
      [](auto &O, auto &) { O.Sat.DefaultPhase = !O.Sat.DefaultPhase; },
      [](auto &, auto &L) { --L.MaxNodes; },
      [](auto &, auto &L) { --L.MaxWork; },
      [](auto &, auto &L) { --L.MaxReachableNodes; },
      [](auto &, auto &L) { --L.MaxWidth; },
      [](auto &, auto &L) { --L.MaxQueries; }};
  for (const auto &Change : Changes) {
    ASSERT_TRUE(Seed());
    SolverOptions O;
    ci::Limits L;
    Change(O, L);
    const auto Result = Cache.proveCached(C, D, D, O, L, [&] {
      ++Q;
      return true;
    });
    EXPECT_FALSE(Result.Proved);
    EXPECT_FALSE(Result.ReusedPreparation);
    EXPECT_EQ(Q, 0U);
  }
}
TEST(ConditionalImplication, UnknownParentNeedsEveryLeafAndTheLastQuery) {
  SymContext C;
  std::vector<SymRef> Ds, Gs;
  for (unsigned I = 0; I < 16; ++I) {
    auto X = C.mkVar("x" + std::to_string(I), 8),
         Y = C.mkVar("y" + std::to_string(I), 8);
    Ds.push_back(C.mkEq(C.mkAdd(X, C.mkConst(8, 1)), Y));
    Gs.push_back(C.mkEq(X, C.mkAdd(Y, C.mkConst(8, 255))));
  }
  auto D = C.mkAnd(Ds), G = C.mkAnd(Gs);
  bool Exercised = false;
  for (unsigned Propagations :
       {32U, 64U, 128U, 256U, 512U, 1024U, 2048U, 4096U}) {
    SolverOptions O;
    O.Sat.MaxPropagations = Propagations;
    ci::Cache Cache;
    unsigned Q = 0;
    const auto R = Cache.proveFresh(C, D, G, O, {}, 100, [&] {
      ++Q;
      return true;
    });
    if (!R.Proved || R.Batches.size() == 1)
      continue;
    Exercised = true;
    EXPECT_EQ(R.Queries, Q);
    std::vector<unsigned> Covered(R.TotalTerms);
    for (const auto &B : R.Batches)
      if (B.Answer == SatResult::Unsat)
        for (unsigned I = B.Begin; I < B.Begin + B.Count; ++I)
          ++Covered[I];
    for (auto Count : Covered)
      EXPECT_EQ(Count, 1U);
    ci::Cache Exact, Short;
    EXPECT_TRUE(Exact.proveFresh(C, D, G, O, {}, R.Queries, [] { return true; })
                    .Proved);
    auto Incomplete =
        Short.proveFresh(C, D, G, O, {}, R.Queries - 1, [] { return true; });
    EXPECT_FALSE(Incomplete.Proved);
    EXPECT_LT(Incomplete.ProvedTerms, Incomplete.TotalTerms);
    // A true prefix of the goal does not discharge a false final conjunct.
    auto Bad =
        C.mkAnd(G, C.mkEq(C.mkExtract(C.varRef(0), 0, 1), C.mkConst(1, 0)));
    ci::Cache Negative;
    EXPECT_FALSE(
        Negative.proveFresh(C, D, Bad, {}, {}, 100, [] { return true; })
            .Proved);
    break;
  }
  EXPECT_TRUE(Exercised);
}
TEST(ConditionalImplication, WorkNodeWidthAndQueryRefusalsNeverComplete) {
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  auto D = C.mkEq(C.mkAdd(X, C.mkConst(8, 1)), Y),
       G = C.mkEq(X, C.mkAdd(Y, C.mkConst(8, 255)));
  ci::Cache Cache;
  unsigned Q = 0;
  EXPECT_FALSE(Cache
                   .proveFresh(C, D, G, {}, {}, 100,
                               [&] {
                                 ++Q;
                                 return false;
                               })
                   .Proved);
  EXPECT_EQ(Q, 1U);
  for (unsigned Case = 0; Case != 5; ++Case) {
    ci::Limits L;
    if (Case == 0)
      L.MaxNodes = C.numNodes() - 1;
    if (Case == 1)
      L.MaxWork = 0;
    if (Case == 2)
      L.MaxReachableNodes = 1;
    if (Case == 3)
      L.MaxWidth = 7;
    if (Case == 4)
      L.MaxQueries = 0;
    Q = 0;
    EXPECT_FALSE(Cache
                     .proveFresh(C, D, G, {}, L, 100,
                                 [&] {
                                   ++Q;
                                   return true;
                                 })
                     .Proved);
    EXPECT_EQ(Q, 0U);
  }
  for (auto Bad : {SymRef{}, SymRef(0xfffffffe), X})
    EXPECT_FALSE(
        Cache.proveFresh(C, Bad, G, {}, {}, 100, [] { return true; }).Proved);
  SolverOptions Tiny;
  Tiny.Blast.MaxGates = 1;
  EXPECT_FALSE(
      Cache.proveFresh(C, D, G, Tiny, {}, 100, [] { return true; }).Proved);
  Tiny = {};
  Tiny.Sat.MaxPropagations = 1;
  EXPECT_FALSE(
      Cache.proveFresh(C, D, G, Tiny, {}, 100, [] { return true; }).Proved);
  ASSERT_TRUE(
      Cache.proveFresh(C, D, G, {}, {}, 100, [] { return true; }).Proved);
  ci::Limits L;
  L.MaxNodes = C.numNodes();
  ci::Cache Growth;
  unsigned GrowthQueries = 0;
  EXPECT_FALSE(Growth
                   .proveFresh(C, D, G, {}, L, 100,
                               [&] {
                                 ++GrowthQueries;
                                 C.mkFreshVar(1);
                                 return true;
                               })
                   .Proved);
  EXPECT_EQ(GrowthQueries, 1U);
}
TEST(ConditionalImplication, ExactAndShortPreparationWorkBoundaries) {
  const auto Run = [](uint64_t Work) {
    SymContext C;
    auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
    auto D = C.mkEq(C.mkAdd(X, C.mkConst(8, 1)), Y),
         G = C.mkEq(X, C.mkAdd(Y, C.mkConst(8, 255)));
    ci::Limits L;
    L.MaxWork = Work;
    ci::Cache Cache;
    return Cache.proveFresh(C, D, G, {}, L, 100, [] { return true; });
  };
  uint64_t First = 0;
  for (uint64_t Work = 1; Work < 1024; ++Work) {
    auto R = Run(Work);
    if (R.Proved) {
      First = Work;
      break;
    }
  }
  ASSERT_GT(First, 0U);
  EXPECT_TRUE(Run(First).Proved);
  EXPECT_FALSE(Run(First - 1).Proved);
  const auto R = Run(First);
  EXPECT_LE(R.NormalizationWork, First);
  EXPECT_LE(R.CollectionWork, First);
  EXPECT_LE(R.ClosureWork, First);
  EXPECT_LE(R.RebuildWork, First);
}
TEST(ConditionalImplication, MalformedOperandsAreRejectedBeforeRebuild) {
  SymContext C;
  auto W = C.mkVar("wide", 16), X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  auto D = C.mkEq(X, Y), G = C.mkTrue();
  auto &Operand = const_cast<SymRef &>(C.operands(D)[0]);
  auto Original = Operand;
  for (auto Bad : {SymRef{}, SymRef(0xfffffffe), D, W}) {
    Operand = Bad;
    ci::Cache Cache;
    EXPECT_FALSE(
        Cache.proveFresh(C, D, G, {}, {}, 100, [] { return true; }).Proved);
    Operand = Original;
  }
}
TEST(ConditionalImplication,
     CachedQueriesCannotRetainEarlierAssumptionsOrUnknown) {
  SymContext C;
  auto D = C.mkTrue(), F = C.mkVar("f", 1);
  ci::Cache Cache;
  ASSERT_TRUE(
      Cache.proveFresh(C, D, D, {}, {}, 100, [] { return true; }).Proved);
  for (unsigned I = 0; I < 3; ++I) {
    EXPECT_FALSE(
        Cache.proveCached(C, D, F, {}, {}, [] { return true; }).Proved);
    EXPECT_FALSE(
        Cache.proveCached(C, D, C.mkNot(F), {}, {}, [] { return true; })
            .Proved);
    EXPECT_TRUE(Cache.proveCached(C, D, D, {}, {}, [] { return true; }).Proved);
    EXPECT_FALSE(
        Cache.proveCached(C, D, C.mkFalse(), {}, {}, [] { return true; })
            .Proved);
  }
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  auto HardFalse = C.mkEq(C.mkMul(X, X), Y);
  SolverOptions Tiny;
  Tiny.Blast.MaxGates = 1;
  ci::Cache Limited;
  ASSERT_TRUE(
      Limited.proveFresh(C, D, D, Tiny, {}, 100, [] { return true; }).Proved);
  const auto R =
      Limited.proveCached(C, D, HardFalse, Tiny, {}, [] { return true; });
  EXPECT_TRUE(R.ReusedPreparation);
  EXPECT_FALSE(R.Proved);
  ASSERT_EQ(R.Batches.size(), 1U);
  EXPECT_EQ(R.Batches[0].Answer, SatResult::Unknown);
  EXPECT_FALSE(
      Limited.proveCached(C, D, D, Tiny, {}, [] { return false; }).Proved);
  ci::Cache Failed;
  EXPECT_FALSE(
      Failed.proveFresh(C, D, HardFalse, {}, {}, 100, [] { return true; })
          .Proved);
  const auto Absent = Failed.proveCached(C, D, D, {}, {}, [] { return true; });
  EXPECT_FALSE(Absent.ReusedPreparation);
  EXPECT_FALSE(Absent.Proved);
  EXPECT_EQ(Absent.Queries, 0U);
}
} // namespace
