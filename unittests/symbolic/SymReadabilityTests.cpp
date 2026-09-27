//===- SymReadabilityTests.cpp - Rendered candidate cost and selection
//------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymBitwise.h"
#include "neverd/symbolic/SymExpr.h"
#include "neverd/symbolic/SymMBA.h"
#include "neverd/symbolic/SymParse.h"

#include <array>
#include <limits>

using namespace neverd::symbolic;

namespace {

TEST(SymReadability, CountsRenderedSignsAndInfixChains) {
  struct Case {
    const char *Text;
    size_t Nodes;
    size_t Operations;
  };
  for (unsigned Width : {3u, 8u, 32u, 64u, 257u}) {
    for (const Case &C : {
             Case{"-1", 1, 0},
             Case{"x-1", 3, 1},
             Case{"x-y", 3, 1},
             Case{"-x-y", 4, 2},
             Case{"-x*y", 4, 2},
             Case{"-2*x-y", 5, 2},
             Case{"y-2*x", 5, 2},
             Case{"x+y+z", 5, 2},
             Case{"x*y*z", 5, 2},
             Case{"x&y&z", 5, 2},
             Case{"x|y|z", 5, 2},
             Case{"x^y^z", 5, 2},
             Case{"x!=y", 3, 1},
             Case{"~(-1+x+y)", 6, 3},
             Case{"-~x", 3, 2},
             Case{"~-x", 3, 1},
             Case{"x+1", 3, 1},
         }) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(C.Text);
      SymContext Ctx;
      auto Parsed = parseSymExpr(Ctx, C.Text, Width);
      ASSERT_TRUE(Parsed.ok());
      EXPECT_EQ(Ctx.readabilityCost(Parsed.Root), C.Nodes);
      EXPECT_EQ(Ctx.readability(Parsed.Root).Operations, C.Operations);
    }
  }
}

TEST(SymReadability, KeepsOneBitAndSignedMinimumLiteralShapes) {
  for (unsigned Width : {1u, 3u, 8u, 64u, 257u}) {
    SymContext C;
    SymRef X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    EXPECT_EQ(C.readability(C.mkOnes(Width)), (SymReadability{1, 0}));
    if (Width == 1) {
      EXPECT_EQ(C.readability(C.mkNeg(X)), (SymReadability{1, 0}));
      continue;
    }
    SymRef Min = C.mkConst(llvm::APInt::getSignedMinValue(Width));
    EXPECT_EQ(C.readability(C.mkMul(Min, X)), (SymReadability{3, 1}));
    EXPECT_EQ(C.readability(C.mkAdd(C.mkMul(Min, X), Y)),
              (SymReadability{5, 2}));
  }
}

TEST(SymReadability, ChargesSharedTextAndPreservesScalarProducts) {
  SymContext C;
  SymRef X = C.mkVar("x", 64), Y = C.mkVar("y", 64);
  SymRef Z = C.mkVar("z", 64), T = C.mkXor(Y, Z);
  SymRef Compact = C.mkNeg(C.mkAnd(C.mkNot(X), T));
  SymRef Repeated = C.mkSub(C.mkAnd(T, X), T);
  EXPECT_EQ(C.readability(Compact), (SymReadability{7, 4}));
  EXPECT_EQ(C.readability(Repeated), (SymReadability{9, 4}));
  EXPECT_LT(C.readability(Compact), C.readability(Repeated));
  SymRef Doubled = C.mkAdd(T, T);
  EXPECT_EQ(Doubled, C.mkMul(C.mkConst(64, 2), T));
  EXPECT_EQ(C.readability(Doubled), (SymReadability{5, 2}));
}

TEST(SymReadability, ExtendsTheCacheWithoutExpandingSharedTrees) {
  SymContext C;
  SymRef X = C.mkVar("x", 64), Y = C.mkVar("y", 64);
  SymRef Root = X;
  size_t Previous = 0;
  for (unsigned I = 0; I != 256; ++I) {
    Root = C.mkUDiv(Root, C.mkXor(Root, Y));
    SymReadability Cost = C.readability(Root);
    EXPECT_GE(Cost.Nodes, Previous);
    Previous = Cost.Nodes;
  }
  const size_t Max = std::numeric_limits<size_t>::max();
  EXPECT_EQ(C.readability(Root), (SymReadability{Max, Max}));
  EXPECT_EQ(C.readability(C.mkSub(Y, Root)), (SymReadability{Max, Max}));
  EXPECT_EQ(C.readability(X), (SymReadability{1, 0}));
  EXPECT_LT(C.dagSize(Root), 520u);
}

TEST(SymReadability, SelectsShorterArithmeticAndFewerOperations) {
  for (unsigned Width : {3u, 8u, 32u, 64u, 128u, 257u})
    for (bool Deep : {false, true})
      for (const auto &[Text, Expected] :
           {std::pair{"~(-1+x+y)", "-x-y"}, std::pair{"-~x", "x+1"},
            std::pair{"(x+1)^y", "(x+1)^y"}, std::pair{"-(y&~x)", "(y&x)-y"}}) {
        SCOPED_TRACE(Width);
        SCOPED_TRACE(Deep);
        SCOPED_TRACE(Text);
        SymContext C;
        auto Input = parseSymExpr(C, Text, Width);
        auto Want = parseSymExpr(C, Expected, Width);
        ASSERT_TRUE(Input.ok());
        ASSERT_TRUE(Want.ok());
        MBAOptions Options;
        Options.VerifySamples = 0;
        auto Result = Deep ? simplifyMBADeep(C, Input.Root, Options)
                           : simplifyMBA(C, Input.Root, Options);
        EXPECT_LE(C.readability(Result.Expr), C.readability(Want.Root))
            << C.toString(Result.Expr);
        EXPECT_LE(C.readability(Result.Expr), C.readability(Input.Root));
        EXPECT_LE(Result.Work, Options.MaxWork);
        if (Result.Changed)
          EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
      }
}

TEST(SymReadability, PreservesNearbySemanticsWithSmallBudgets) {
  for (const char *Text :
       {"~(-1+x+y)", "~(-2+x+y)", "-~x^y", "-(y&~x)", "-(y|~x)", "-(~x&(y^z))"})
    for (bool Deep : {false, true})
      for (size_t Work : {size_t(0), size_t(64), MBAOptions{}.MaxWork}) {
        SCOPED_TRACE(Text);
        SCOPED_TRACE(Deep);
        SCOPED_TRACE(Work);
        SymContext C;
        auto Input = parseSymExpr(C, Text, 3);
        ASSERT_TRUE(Input.ok());
        MBAOptions Options;
        Options.VerifySamples = 0;
        Options.MaxWork = Work;
        auto Result = Deep ? simplifyMBADeep(C, Input.Root, Options)
                           : simplifyMBA(C, Input.Root, Options);
        EXPECT_LE(C.readability(Result.Expr), C.readability(Input.Root));
        EXPECT_LE(Result.Work, Work);
        for (uint64_t X = 0; X != 8; ++X)
          for (uint64_t Y = 0; Y != 8; ++Y)
            for (uint64_t Z = 0; Z != 8; ++Z) {
              std::array<uint64_t, 3> Values{X, Y, Z};
              ASSERT_EQ(C.evalU64(Input.Root, Values),
                        C.evalU64(Result.Expr, Values));
            }
      }
}

TEST(SymReadability, KeepsComplementedProductsOnAnExactScoreTie) {
  for (unsigned Width : {3u, 8u, 64u, 128u, 257u})
    for (bool Deep : {false, true}) {
      SymContext C;
      SymRef X = C.mkVar("x", Width);
      SymRef Input = C.mkNot(C.mkMul(C.mkConst(Width, 2), X));
      SymRef Alternative = C.mkSub(C.mkNot(X), X);
      ASSERT_EQ(C.readability(Input), C.readability(Alternative));
      MBAOptions Options;
      Options.VerifySamples = 0;
      auto Result = Deep ? simplifyMBADeep(C, Input, Options)
                         : simplifyMBA(C, Input, Options);
      EXPECT_EQ(Result.Expr, Input);
      EXPECT_LE(Result.Work, 4096u);
    }
}

TEST(SymReadability, EnforcesBitwiseLiteralCostLimit) {
  for (bool Ones : {false, true}) {
    SymContext C;
    auto Table = Ones ? TruthTable::ones(1) : TruthTable::zero(1);
    SymRef X = C.mkVar("x", 64);
    BitwiseSynthesisLimits Limits;
    Limits.MaxCost = 0;
    EXPECT_FALSE(synthesizeBitwise(C, Table, {X}, Limits).has_value());
    Limits.MaxCost = 1;
    EXPECT_TRUE(synthesizeBitwise(C, Table, {X}, Limits).has_value());
  }
}

TEST(SymReadability, CostsBitwiseRecipesAfterRestoringRelatedAtoms) {
  for (unsigned Width : {1u, 8u, 64u, 257u}) {
    SymContext C;
    SymRef X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    SymRef A = C.mkAdd({X, Y, C.mkVar("z", Width)});
    auto Table = TruthTable::ones(3);
    Table.setValue(0, false);
    BitwiseSynthesisLimits Limits;
    Limits.MaxCost = 4;
    auto Repeated = synthesizeBitwise(C, Table, {X, X, Y}, Limits);
    ASSERT_TRUE(Repeated.has_value());
    EXPECT_EQ(*Repeated, C.mkOr(X, Y));
    Limits.MaxCost = 10;
    auto Compound = synthesizeBitwise(C, Table, {A, A, X}, Limits);
    ASSERT_TRUE(Compound.has_value());
    EXPECT_EQ(*Compound, C.mkOr(A, X));
    Limits.MaxCost = 1;
    auto Complement = synthesizeBitwise(C, Table, {X, C.mkNot(X), Y}, Limits);
    ASSERT_TRUE(Complement.has_value());
    EXPECT_EQ(*Complement, C.mkOnes(Width));
  }
}

} // namespace
