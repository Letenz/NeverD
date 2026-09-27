//===- SymMBAArithmeticTests.cpp - Modular arithmetic regions ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "SymMBATestsDetail.h"

namespace {
using namespace neverd::symbolic;

TEST(SymMBAArithmetic, CancelsDistributedProducts) {
  for (uint32_t Width : {8u, 16u, 32u, 64u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    test::simplifiesTo("(x + y) * (x - y) - x * x + y * y", "0", Width);
    test::simplifiesTo("(x + y) * z - x * z - y * z", "0", Width);
    test::simplifiesTo("(x + 1) * (x + 1) - x * x - 2 * x", "1", Width);
  }
}

TEST(SymMBAArithmetic, ExtractsSharedFactors) {
  for (uint32_t Width : {8u, 32u, 128u}) {
    SCOPED_TRACE(Width);
    test::simplifiesTo("x * y + x * z", "x * (y + z)", Width);
    test::simplifiesTo("x * x * y + x * x * z", "x * x * (y + z)", Width);
    test::simplifiesTo("x * y + x * z + w", "x * (y + z) + w", Width);
  }
}

TEST(SymMBAArithmetic, PreservesOpaqueOperationsAsExactAtoms) {
  test::simplifiesTo("(x / z) * (y + 1) - (x / z) * y", "x / z", 32);
  test::simplifiesTo("(x & z) * (y + 1) - (x & z) * y", "x & z", 32);
  test::simplifiesTo("(x >> z) * (y + 1) - (x >> z) * y", "x >> z", 32);
}

TEST(SymMBAArithmetic, CoefficientsWrapAtTheDeclaredWidth) {
  test::simplifiesTo("128 * (x + y) * z + 128 * x * z + 128 * y * z", "0", 8);
  test::simplifiesTo("255 * (x + y) * z + x * z + y * z", "0", 8);
}

TEST(SymMBAArithmetic, ShallowAndDeepKeepTheOriginalPolynomialOpportunity) {
  for (bool Deep : {false, true}) {
    for (const char *Text : {"(x+1)*(x+1)-x*x-2*x", "(x+y)*(x-y)-x*x+y*y+1"}) {
      SymContext Ctx;
      auto P = parseSymExpr(Ctx, Text, 32);
      ASSERT_TRUE(P.ok());
      MBAResult R =
          Deep ? simplifyMBADeep(Ctx, P.Root) : simplifyMBA(Ctx, P.Root);
      EXPECT_EQ(R.Expr, Ctx.mkOne(32)) << Text << ": " << Ctx.toString(R.Expr);
      EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
    }
  }
}

TEST(SymMBAArithmetic, SkipsOrdinaryLinearRegionsWithoutSpendingWork) {
  SymContext Ctx;
  auto P = parseSymExpr(Ctx, "(x ^ y) + 2 * (x & y)", 32);
  ASSERT_TRUE(P.ok());
  MBAOptions Opts;
  detail::WorkBudget Budget(0);
  detail::SolveReport Report;
  EXPECT_EQ(detail::solveArithmetic(Ctx, P.Root, Opts, Budget, Report), P.Root);
  EXPECT_EQ(Budget.used(), 0u);
  EXPECT_FALSE(Report.BudgetExhausted);
}

TEST(SymMBAArithmetic, WorkAndStorageLimitsKeepTheOriginalExpression) {
  for (bool LimitStorage : {false, true}) {
    SymContext Ctx;
    auto P = parseSymExpr(Ctx, "(x+y)*(x-y)-x*x+y*y", 128);
    ASSERT_TRUE(P.ok());
    MBAOptions Opts;
    if (LimitStorage)
      Opts.MaxTableBytes = 128;
    else
      Opts.MaxWork = 10;
    detail::WorkBudget Budget(Opts.MaxWork);
    detail::SolveReport Report;
    const size_t Nodes = Ctx.numNodes();
    EXPECT_EQ(detail::solveArithmetic(Ctx, P.Root, Opts, Budget, Report),
              P.Root);
    EXPECT_TRUE(Report.BudgetExhausted);
    EXPECT_EQ(Report.Outcome, MBAOutcome::BudgetExhausted);
    EXPECT_EQ(Ctx.numNodes(), Nodes);
    EXPECT_LE(Budget.used(), Opts.MaxWork);
  }
}

TEST(SymMBAArithmetic, SharedExpansionStopsBeforeAllocatingAnUnboundedTable) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 32);
  SymRef Y = Ctx.mkVar("y", 32);
  SymRef E = Ctx.mkAdd(X, Y);
  for (unsigned I = 0; I != 16; ++I)
    E = Ctx.mkAdd(Ctx.mkMul(E, E), Ctx.mkOne(32));
  MBAOptions Opts;
  Opts.MaxWork = 5000;
  detail::WorkBudget Budget(Opts.MaxWork);
  detail::SolveReport Report;
  const size_t Nodes = Ctx.numNodes();
  EXPECT_EQ(detail::solveArithmetic(Ctx, E, Opts, Budget, Report), E);
  EXPECT_TRUE(Report.BudgetExhausted);
  EXPECT_EQ(Ctx.numNodes(), Nodes);
  EXPECT_LE(Budget.used(), Opts.MaxWork);
}

TEST(SymMBAArithmetic, NormalizesDeepArithmeticWithoutRecursiveTraversal) {
  SymContext Ctx;
  SymRef E = Ctx.mkVar("x", 32);
  SymRef One = Ctx.mkOne(32);
  SymRef Two = Ctx.mkConst(llvm::APInt(32, 2));
  for (unsigned I = 0; I != 10000; ++I)
    E = Ctx.mkMul(Two, Ctx.mkAdd(E, One));
  MBAOptions Opts;
  detail::WorkBudget Budget(Opts.MaxWork);
  detail::SolveReport Report;
  SymRef R = detail::solveArithmetic(Ctx, E, Opts, Budget, Report);
  EXPECT_EQ(R, Ctx.mkConst(-llvm::APInt(32, 2)));
  EXPECT_EQ(Report.Evidence, MBAEvidence::Derivation);
  EXPECT_FALSE(Report.BudgetExhausted);
}

TEST(SymMBAArithmetic, DoesNotExpandAnAlreadyFactoredProduct) {
  SymContext Ctx;
  auto P = parseSymExpr(Ctx, "(x+y)*(z+w)", 32);
  ASSERT_TRUE(P.ok());
  MBAOptions Opts;
  Opts.AllowGrowth = true;
  detail::WorkBudget Budget(Opts.MaxWork);
  detail::SolveReport Report;
  EXPECT_EQ(detail::solveArithmetic(Ctx, P.Root, Opts, Budget, Report), P.Root);
}

TEST(SymMBAArithmetic, DerivedFormsAgreeAtEverySmallWidthAssignment) {
  for (const char *Text :
       {"(x+y)*(x-y)-x*x+y*y", "(x+y)*z-x*z-y*z", "x*x*y+x*x*z+x*z",
        "7*(x+y)*z+x*z+3*y*z", "(x/y)*(z+1)-(x/y)*z"}) {
    SymContext Ctx;
    auto P = parseSymExpr(Ctx, Text, 4);
    ASSERT_TRUE(P.ok());
    MBAOptions Opts;
    detail::WorkBudget Budget(Opts.MaxWork);
    detail::SolveReport Report;
    SymRef R = detail::solveArithmetic(Ctx, P.Root, Opts, Budget, Report);
    EXPECT_LE(Ctx.readabilityCost(R), Ctx.readabilityCost(P.Root));
    llvm::SmallVector<uint64_t, 3> Values(Ctx.numVars(), 0);
    const uint64_t Assignments = uint64_t(1) << (4 * Ctx.numVars());
    for (uint64_t I = 0; I != Assignments; ++I) {
      for (size_t J = 0; J != Values.size(); ++J)
        Values[J] = (I >> (4 * J)) & 15;
      ASSERT_EQ(Ctx.evalU64(P.Root, Values), Ctx.evalU64(R, Values))
          << Text << " at " << I;
    }
  }
}
} // namespace
