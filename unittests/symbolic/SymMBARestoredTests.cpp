//===- SymMBARestoredTests.cpp - Restored region refinement ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "SymMBATestsDetail.h"

namespace {
using namespace neverd::symbolic;

SymRef carrySum(SymContext &Ctx, SymRef X, SymRef Y) {
  return Ctx.mkAdd(Ctx.mkXor(X, Y),
                   Ctx.mkMul(Ctx.mkConst(Ctx.width(X), 2), Ctx.mkAnd(X, Y)));
}

TEST(SymMBARestored, FinishesNewLinearOpportunityInOneCall) {
  for (unsigned Width : {1u, 3u, 8u, 32u, 64u, 128u, 256u}) {
    for (bool Deep : {false, true}) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Deep);
      SymContext Ctx;
      SymRef X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
      SymRef FourY = Ctx.mkMul(Ctx.mkConst(Width, 4), Y);
      SymRef Input = carrySum(Ctx, Ctx.mkXor(X, Y), Ctx.mkAdd(X, FourY));
      SymRef Expected =
          Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(Width, 3), Y),
                    Ctx.mkMul(Ctx.mkConst(Width, 2), Ctx.mkOr(X, Y)));
      MBAOptions Opts;
      Opts.VerifySamples = 0;
      MBAResult R = Deep ? simplifyMBADeep(Ctx, Input, Opts)
                         : simplifyMBA(Ctx, Input, Opts);
      EXPECT_EQ(R.Expr, Expected) << Ctx.toString(R.Expr);
      EXPECT_LE(R.SizeAfter, R.SizeBefore);
      EXPECT_LE(R.Work, Opts.MaxWork);
      if (Width > 1)
        EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
    }
  }
}

TEST(SymMBARestored, PreservesNearbyNonidentitiesWithoutSamples) {
  for (unsigned Width : {1u, 3u, 4u}) {
    for (unsigned Carry : {1u, 2u, 3u}) {
      SymContext Ctx;
      SymRef X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
      SymRef P = Ctx.mkAdd(X, Ctx.mkMul(Ctx.mkConst(Width, 4), Y));
      SymRef A = Ctx.mkXor(X, Y);
      SymRef Input =
          Ctx.mkAdd(Ctx.mkXor(A, P),
                    Ctx.mkMul(Ctx.mkConst(Width, Carry), Ctx.mkAnd(A, P)));
      MBAOptions Opts;
      Opts.VerifySamples = 0;
      MBAResult R = simplifyMBADeep(Ctx, Input, Opts);
      SymEvalPlan Before(Ctx, Input), After(Ctx, R.Expr);
      llvm::SmallVector<uint64_t, 8> Values(Ctx.numVars(), 0);
      for (unsigned XV = 0; XV < (1u << Width); ++XV)
        for (unsigned YV = 0; YV < (1u << Width); ++YV) {
          Values[Ctx.varId(X)] = XV;
          Values[Ctx.varId(Y)] = YV;
          EXPECT_EQ(Before.evalU64(Values), After.evalU64(Values));
        }
    }
  }
}

TEST(SymMBARestored, SharesTheCallersRemainingWorkAndKeepsProvedProgress) {
  for (size_t Limit : {0u, 1u, 32u, 128u, 512u, 1024u, 4096u}) {
    SymContext Ctx;
    auto P =
        parseSymExpr(Ctx, "((x ^ y) ^ (x + 4*y)) + 2*((x ^ y) & (x + 4*y))", 4);
    ASSERT_TRUE(P.ok());
    MBAOptions Opts;
    Opts.VerifySamples = 0;
    Opts.MaxWork = Limit;
    detail::WorkBudget Budget(Limit);
    ASSERT_TRUE(Budget.consume(Limit / 2));
    detail::SolveReport Report;
    SymRef R = detail::solveOneRegion(Ctx, P.Root, Opts, Budget, Report);
    EXPECT_LE(Budget.used(), Limit);
    EXPECT_LE(Ctx.readabilityCost(R), Ctx.readabilityCost(P.Root));
    SymEvalPlan Before(Ctx, P.Root), After(Ctx, R);
    llvm::SmallVector<uint64_t, 8> Values(Ctx.numVars(), 0);
    for (unsigned I = 0; I < 256; ++I) {
      Values[0] = I & 15;
      Values[1] = I >> 4;
      EXPECT_EQ(Before.evalU64(Values), After.evalU64(Values));
    }
    if (Limit <= 1)
      EXPECT_TRUE(Report.BudgetExhausted);
  }
}

TEST(SymMBARestored, DoesNotRevisitMinimalVariableSums) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 32), Y = Ctx.mkVar("y", 32);
  SymRef Input = carrySum(Ctx, X, Y);
  MBAOptions Opts;
  Opts.VerifySamples = 0;
  MBAResult R = simplifyMBA(Ctx, Input, Opts);
  EXPECT_EQ(R.Expr, Ctx.mkAdd(X, Y));
  MBAResult Again = simplifyMBA(Ctx, R.Expr, Opts);
  EXPECT_EQ(Again.Expr, R.Expr);
  EXPECT_EQ(Again.Work, Ctx.dagSize(R.Expr));
}

TEST(SymMBARestored, GrowthModeStillAcceptsTheShorterRestoredForm) {
  SymContext Ctx;
  auto P =
      parseSymExpr(Ctx, "((x ^ y) ^ (x + 4*y)) + 2*((x ^ y) & (x + 4*y))", 64);
  auto Expected = parseSymExpr(Ctx, "3*y + 2*(x|y)", 64);
  ASSERT_TRUE(P.ok());
  ASSERT_TRUE(Expected.ok());
  MBAOptions Opts;
  Opts.VerifySamples = 0;
  Opts.AllowGrowth = true;
  MBAResult R = simplifyMBA(Ctx, P.Root, Opts);
  EXPECT_EQ(R.Expr, Expected.Root);
  EXPECT_LT(R.SizeAfter, R.SizeBefore);
}

TEST(SymMBARestored, StorageRefusalKeepsTheOriginalExpression) {
  SymContext Ctx;
  auto P =
      parseSymExpr(Ctx, "((x ^ y) ^ (x + 4*y)) + 2*((x ^ y) & (x + 4*y))", 256);
  ASSERT_TRUE(P.ok());
  MBAOptions Opts = MBAOptions::unlimited();
  Opts.VerifySamples = 0;
  Opts.MaxTableBytes = 0;
  MBAResult R = simplifyMBA(Ctx, P.Root, Opts);
  EXPECT_EQ(R.Expr, P.Root);
  EXPECT_FALSE(R.Changed);
}

TEST(SymMBARestored, RefinesInputsWithASharedOpaqueTail) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 32), Y = Ctx.mkVar("y", 32);
  SymRef Tail = X;
  for (unsigned I = 0; I < 1000; ++I)
    Tail = Ctx.mkUDiv(Tail, Y);
  SymRef Input = carrySum(Ctx, Ctx.mkXor(X, Tail),
                          Ctx.mkAdd(X, Ctx.mkMul(Ctx.mkConst(32, 4), Tail)));
  SymRef Expected = Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(32, 3), Tail),
                              Ctx.mkMul(Ctx.mkConst(32, 2), Ctx.mkOr(X, Tail)));
  MBAOptions Opts;
  Opts.VerifySamples = 0;
  const size_t Nodes = Ctx.numNodes();
  MBAResult R = simplifyMBADeep(Ctx, Input, Opts);
  EXPECT_EQ(R.Expr, Expected);
  EXPECT_LE(R.Work, Opts.MaxWork);
  EXPECT_LT(Ctx.numNodes() - Nodes, 2 * Nodes);
}

TEST(SymMBARestored, DeepSharedOpaqueTailRemainsIterativeAndBounded) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 32), Y = Ctx.mkVar("y", 32);
  SymRef Tail = X;
  for (unsigned I = 0; I < 10000; ++I)
    Tail = Ctx.mkUDiv(Tail, Y);
  SymRef Input = carrySum(Ctx, Ctx.mkXor(X, Tail),
                          Ctx.mkAdd(X, Ctx.mkMul(Ctx.mkConst(32, 4), Tail)));
  MBAOptions Opts;
  Opts.VerifySamples = 0;
  Opts.MaxWork = 128;
  const size_t Nodes = Ctx.numNodes();
  MBAResult R = simplifyMBADeep(Ctx, Input, Opts);
  EXPECT_EQ(R.Expr, Input);
  EXPECT_LE(R.Work, Opts.MaxWork);
  EXPECT_EQ(R.Outcome, MBAOutcome::BudgetExhausted);
  EXPECT_LT(Ctx.numNodes() - Nodes, 64u);
}
} // namespace
