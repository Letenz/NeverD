//===- SymMBAComplementArithmeticTests.cpp - Word complements ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/symbolic/SymParse.h"

#include <array>

using namespace neverd::symbolic;

namespace {

TEST(SymMBAComplementArithmetic, CancelsAcrossSumsAndProducts) {
  const std::pair<const char *, const char *> Cases[] = {
      {"-1-~x+(y^z)", "x+(y^z)"}, {"3*~x+3*x+(y^z)", "-3+(y^z)"},
      {"x*y+x*~y", "-x"},         {"x*y+x*~(y+1)", "-2*x"},
      {"(x/z)*y+~(x/z)*y", "-y"}, {"~(x+y)+x+y+z*w", "z*w-1"}};
  for (unsigned W : {1u, 3u, 8u, 32u, 64u, 128u, 257u}) {
    for (const auto &[InputText, ExpectedText] : Cases) {
      SCOPED_TRACE(W);
      SCOPED_TRACE(InputText);
      SymContext C;
      auto Input = parseSymExpr(C, InputText, W);
      auto Expected = parseSymExpr(C, ExpectedText, W);
      ASSERT_TRUE(Input.ok());
      ASSERT_TRUE(Expected.ok());
      MBAOptions Options;
      Options.VerifySamples = 0;
      detail::WorkBudget Budget(Options.MaxWork);
      detail::SolveReport Report;
      SymRef Result =
          detail::solveArithmetic(C, Input.Root, Options, Budget, Report);
      EXPECT_EQ(Result, Expected.Root) << C.toString(Result);
      if (Result != Input.Root)
        EXPECT_EQ(Report.Evidence, MBAEvidence::Derivation);
    }
  }
}

TEST(SymMBAComplementArithmetic, LeavesBitwiseConsumersOpaque) {
  for (unsigned W : {3u, 64u, 257u}) {
    SymContext C;
    auto Input = parseSymExpr(C, "(x&~y)*(z+1)-(x&~y)*z", W);
    auto Expected = parseSymExpr(C, "x&~y", W);
    ASSERT_TRUE(Input.ok());
    ASSERT_TRUE(Expected.ok());
    MBAOptions Options;
    Options.VerifySamples = 0;
    detail::WorkBudget Budget(Options.MaxWork);
    detail::SolveReport Report;
    EXPECT_EQ(detail::solveArithmetic(C, Input.Root, Options, Budget, Report),
              Expected.Root);
  }
}

TEST(SymMBAComplementArithmetic, PreservesNearbyNonidentitiesExhaustively) {
  for (const char *Text : {"-2-~x+(y^z)", "x*y+x*~(y+1)", "x*~y+~x*y",
                           "~(x*y)+x*y+z", "x*~(y+z)+x*y+x*z", "x&~(y+z)"}) {
    SymContext C;
    auto Input = parseSymExpr(C, Text, 3);
    ASSERT_TRUE(Input.ok());
    MBAOptions Options;
    Options.VerifySamples = 0;
    detail::WorkBudget Budget(Options.MaxWork);
    detail::SolveReport Report;
    SymRef Result =
        detail::solveArithmetic(C, Input.Root, Options, Budget, Report);
    llvm::SmallVector<uint64_t, 3> Values(C.numVars(), 0);
    const uint64_t Count = uint64_t(1) << (3 * C.numVars());
    for (uint64_t I = 0; I < Count; ++I) {
      for (size_t J = 0; J < Values.size(); ++J)
        Values[J] = (I >> (3 * J)) & 7;
      ASSERT_EQ(C.evalU64(Result, Values), C.evalU64(Input.Root, Values))
          << Text << " at " << I;
    }
    EXPECT_LE(C.readabilityCost(Result), C.readabilityCost(Input.Root));
  }
}

TEST(SymMBAComplementArithmetic, RefusesIncompleteWorkAndWideStorage) {
  for (size_t Limit : {size_t(0), size_t(1), size_t(16), size_t(64)}) {
    SymContext C;
    auto Input = parseSymExpr(C, "x*y+x*~y", 128);
    ASSERT_TRUE(Input.ok());
    MBAOptions Options;
    Options.VerifySamples = 0;
    Options.MaxWork = Limit;
    detail::WorkBudget Budget(Limit);
    detail::SolveReport Report;
    EXPECT_EQ(detail::solveArithmetic(C, Input.Root, Options, Budget, Report),
              Input.Root);
    EXPECT_TRUE(Report.BudgetExhausted);
    EXPECT_LE(Budget.used(), Limit);
  }
  SymContext C;
  auto Input = parseSymExpr(C, "x*y+x*~y", 8192);
  ASSERT_TRUE(Input.ok());
  MBAOptions Options;
  Options.MaxTableBytes = 128;
  detail::WorkBudget Budget(Options.MaxWork);
  detail::SolveReport Report;
  const size_t Nodes = C.numNodes();
  EXPECT_EQ(detail::solveArithmetic(C, Input.Root, Options, Budget, Report),
            Input.Root);
  EXPECT_TRUE(Report.BudgetExhausted);
  EXPECT_EQ(C.numNodes(), Nodes);
}

TEST(SymMBAComplementArithmetic, WalksDeepComplementChainsIteratively) {
  SymContext C;
  SymRef X = C.mkVar("x", 64), One = C.mkOne(64), E = X;
  for (unsigned I = 0; I != 2048; ++I)
    E = C.mkNot(C.mkAdd(E, One));
  E = C.mkAdd(C.mkAdd(E, C.mkNot(X)), One);
  MBAOptions Options;
  Options.VerifySamples = 0;
  detail::WorkBudget Budget(Options.MaxWork);
  detail::SolveReport Report;
  EXPECT_EQ(detail::solveArithmetic(C, E, Options, Budget, Report),
            C.mkZero(64));
  EXPECT_FALSE(Report.BudgetExhausted);
}

TEST(SymMBAComplementArithmetic, ReusesSharedComplementPolynomials) {
  for (unsigned Count : {8u, 64u}) {
    SymContext C;
    SymRef T = C.mkXor(C.mkVar("x", 128), C.mkVar("y", 128));
    SymRef A = C.mkAdd(T, C.mkOne(128)), B = C.mkNot(T);
    llvm::SmallVector<SymRef, 32> Terms;
    for (unsigned I = 0; I != Count; ++I) {
      SymRef V = C.mkVar("v" + std::to_string(I), 128);
      Terms.push_back(C.mkMul(V, A));
      Terms.push_back(C.mkMul(V, B));
    }
    SymRef Input = C.mkAdd(Terms);
    MBAOptions Options;
    Options.VerifySamples = 0;
    Options.MaxWork = 100000;
    detail::WorkBudget Budget(Options.MaxWork);
    detail::SolveReport Report;
    EXPECT_EQ(detail::solveArithmetic(C, Input, Options, Budget, Report),
              C.mkZero(128));
    EXPECT_FALSE(Report.BudgetExhausted);
    EXPECT_LE(Budget.used(), Options.MaxWork);
  }
}

TEST(SymMBAComplementArithmetic, DefersCompoundOnlyComplementsToLinearRegions) {
  for (const char *Text : {"z+~(x+y)", "z+3*~(x+y)", "~(x+y)+x+y"}) {
    SymContext C;
    auto Input = parseSymExpr(C, Text, 64);
    ASSERT_TRUE(Input.ok());
    MBAOptions Options;
    detail::WorkBudget Budget(Options.MaxWork);
    detail::SolveReport Report;
    const size_t Nodes = C.numNodes();
    EXPECT_EQ(detail::solveArithmetic(C, Input.Root, Options, Budget, Report),
              Input.Root);
    EXPECT_EQ(Budget.used(), 0u);
    EXPECT_EQ(C.numNodes(), Nodes);
    EXPECT_FALSE(Report.BudgetExhausted);
  }
  SymContext C;
  auto Input = parseSymExpr(C, "~(x+y)+x+y", 64);
  ASSERT_TRUE(Input.ok());
  MBAOptions Options;
  Options.VerifySamples = 0;
  EXPECT_EQ(simplifyMBADeep(C, Input.Root, Options).Expr, C.mkOnes(64));
}

TEST(SymMBAComplementArithmetic, KeepsLayeredLinearComplementWorkBounded) {
  for (unsigned Depth : {8u, 32u, 64u}) {
    SymContext C;
    SymRef Variables[] = {C.mkVar("x", 64), C.mkVar("y", 64), C.mkVar("z", 64),
                          C.mkVar("w", 64)};
    SymRef Input = Variables[0];
    for (unsigned I = 0; I != Depth; ++I)
      Input = C.mkNot(C.mkAdd(Variables[I % 4], Input));
    MBAOptions Options;
    Options.VerifySamples = 0;
    MBAResult Result = simplifyMBADeep(C, Input, Options);
    EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
    EXPECT_LE(Result.Work, 400u * (Depth + 1)) << Depth;
    EXPECT_LT(Result.SizeAfter, Result.SizeBefore);
    for (const std::array<uint64_t, 4> Values :
         {std::array<uint64_t, 4>{0, 0, 0, 0},
          {1, 2, 3, 4},
          {UINT64_MAX, UINT64_MAX - 1, 1, 0},
          {uint64_t(1) << 63, 1, uint64_t(1) << 62, 7}})
      EXPECT_EQ(C.evalU64(Result.Expr, Values), C.evalU64(Input, Values));
  }
}

} // namespace
