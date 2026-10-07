//===- CompletedQueryCacheTests.cpp - Complete context-local answers
//-------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/analysis/core/CompletedQueryCache.h"
#include "gtest/gtest.h"

#include "neverd/solver/BitVectorSolver.h"

#include <array>
#include <type_traits>

using namespace neverd::analysis::detail;
using namespace neverd::symbolic;
using namespace neverd::solver;

namespace {
TEST(CompletedQueryCache, CompleteAnswersMatchIndependentByteDomain) {
  SymContext Ctx;
  const auto X = Ctx.mkVar("x", 8);
  CompletedQueryCache Cache(Ctx, 4096);
  struct Query {
    SymRef P;
    SatResult Answer;
  };
  std::vector<Query> Queries;
  for (unsigned I = 0; I != 64; ++I) {
    const auto A =
        Ctx.mkEq(Ctx.mkAnd(X, Ctx.mkConst(8, 63)), Ctx.mkConst(8, I));
    for (bool Impossible : {false, true}) {
      const auto P = Ctx.mkAnd(
          A,
          Ctx.mkEq(X, Ctx.mkConst(8, Impossible ? ((I + 1) & 63) : (I + 128))));
      unsigned Satisfying = 0;
      for (unsigned V = 0; V != 256; ++V) {
        std::array<llvm::APInt, 1> Values{llvm::APInt(8, V)};
        Satisfying += !Ctx.eval(P, Values).isZero();
      }
      const auto Expected = Satisfying ? SatResult::Sat : SatResult::Unsat;
      ASSERT_EQ(checkSat(Ctx, P), Expected);
      if (const auto Existing = Cache.lookup(Ctx, P))
        ASSERT_EQ(*Existing, Expected);
      ASSERT_TRUE(Cache.store(Ctx, P, Expected));
      Queries.push_back({P, Expected});
    }
  }
  for (auto I = Queries.rbegin(); I != Queries.rend(); ++I)
    EXPECT_EQ(Cache.lookup(Ctx, I->P), I->Answer);
}

TEST(CompletedQueryCache, EveryPackedSlotAndGrowthPreservesNeighbours) {
  SymContext Ctx;
  CompletedQueryCache Cache(Ctx, 513);
  std::vector<SymRef> Predicates;
  while (Ctx.numNodes() < 513) {
    const auto P = Ctx.mkFreshVar(1);
    const auto Answer = P.index() % 2 ? SatResult::Sat : SatResult::Unsat;
    ASSERT_TRUE(Cache.store(Ctx, P, Answer));
    Predicates.push_back(P);
    ASSERT_LE(Cache.allocatedWords(), 17u);
    for (auto Previous : Predicates)
      ASSERT_EQ(Cache.lookup(Ctx, Previous),
                Previous.index() % 2 ? SatResult::Sat : SatResult::Unsat);
  }
  const auto Over = Ctx.mkFreshVar(1);
  EXPECT_FALSE(Cache.store(Ctx, Over, SatResult::Sat));
  EXPECT_FALSE(Cache.lookup(Ctx, Over));
}

TEST(CompletedQueryCache, ForeignContextAndReplacedOwnersMiss) {
  SymContext First, Second;
  const auto P = First.mkFreshVar(1), Q = Second.mkFreshVar(1);
  ASSERT_EQ(P.index(), Q.index());
  std::optional<CompletedQueryCache> Cache;
  Cache.emplace(First, 64);
  ASSERT_TRUE(Cache->store(First, P, SatResult::Sat));
  EXPECT_FALSE(Cache->lookup(Second, Q));
  EXPECT_FALSE(Cache->store(Second, Q, SatResult::Unsat));
  EXPECT_EQ(Cache->lookup(First, P), SatResult::Sat);
  Cache.reset();
  Cache.emplace(Second, 64);
  EXPECT_FALSE(Cache->lookup(Second, Q));
  ASSERT_TRUE(Cache->store(Second, Q, SatResult::Unsat));
  EXPECT_FALSE(Cache->lookup(First, P));
  static_assert(!std::is_copy_constructible_v<CompletedQueryCache>);
  static_assert(!std::is_move_constructible_v<CompletedQueryCache>);
}

TEST(CompletedQueryCache, InvalidAndIncompleteInputsNeverAllocateOrOverwrite) {
  SymContext Ctx;
  const auto P = Ctx.mkFreshVar(1), Wide = Ctx.mkFreshVar(8);
  CompletedQueryCache Cache(Ctx, 128);
  for (auto Invalid :
       {SymRef{}, SymRef{UINT32_MAX},
        SymRef{static_cast<uint32_t>(Ctx.numNodes() + 3)}, Wide}) {
    EXPECT_FALSE(Cache.lookup(Ctx, Invalid));
    EXPECT_FALSE(Cache.store(Ctx, Invalid, SatResult::Sat));
  }
  for (auto Incomplete : {SatResult::Unknown, SatResult::Invalid}) {
    EXPECT_FALSE(Cache.store(Ctx, P, Incomplete));
    EXPECT_FALSE(Cache.lookup(Ctx, P));
  }
  EXPECT_EQ(Cache.allocatedWords(), 0u);
  ASSERT_TRUE(Cache.store(Ctx, P, SatResult::Sat));
  EXPECT_FALSE(Cache.store(Ctx, P, SatResult::Unsat));
  EXPECT_FALSE(Cache.store(Ctx, P, SatResult::Unknown));
  EXPECT_EQ(Cache.lookup(Ctx, P), SatResult::Sat);
}

TEST(CompletedQueryCache, ZeroAndExactNodeCeilingsRemainIndependent) {
  SymContext Ctx;
  const auto P = Ctx.mkFreshVar(1);
  CompletedQueryCache Zero(Ctx, 0), Short(Ctx, P.index()),
      Exact(Ctx, P.index() + 1);
  EXPECT_FALSE(Zero.store(Ctx, P, SatResult::Sat));
  EXPECT_FALSE(Short.store(Ctx, P, SatResult::Sat));
  ASSERT_TRUE(Exact.store(Ctx, P, SatResult::Sat));
  EXPECT_EQ(Exact.lookup(Ctx, P), SatResult::Sat);
  EXPECT_EQ(Zero.allocatedWords(), 0u);
  EXPECT_EQ(Short.allocatedWords(), 0u);
  EXPECT_EQ(Exact.allocatedWords(), P.index() / 32 + 1);
}

TEST(CompletedQueryCache, SolverRefusalsCannotAnswerLaterRequests) {
  SymContext Ctx;
  const auto X = Ctx.mkFreshVar(64), Y = Ctx.mkFreshVar(64);
  const auto P = Ctx.mkEq(Ctx.mkAdd(X, Y), Ctx.mkConst(64, 12345));
  SolverOptions Limits;
  Limits.Blast.MaxGates = 1;
  const auto Unknown = checkSat(Ctx, P, nullptr, Limits);
  ASSERT_EQ(Unknown, SatResult::Unknown);
  CompletedQueryCache Cache(Ctx, 4096);
  EXPECT_FALSE(Cache.store(Ctx, P, Unknown));
  EXPECT_FALSE(Cache.lookup(Ctx, P));
  const auto Complete = checkSat(Ctx, P);
  ASSERT_EQ(Complete, SatResult::Sat);
  ASSERT_TRUE(Cache.store(Ctx, P, Complete));
  EXPECT_EQ(Cache.lookup(Ctx, P), Complete);
}
} // namespace
