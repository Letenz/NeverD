//===- SymExprComplementTests.cpp - Local modular complements ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymExpr.h"
#include "neverd/symbolic/SymParse.h"

#include <array>
#include <utility>

using namespace neverd::symbolic;

TEST(SymExprComplement, RecoversShorterAffineWordSpellings) {
  const std::pair<const char *, const char *> Cases[] = {
      {"~(-1-x-y)", "x+y"},
      {"~(-1+2*x-4*y)", "-2*x+4*y"},
      {"~(-1-x+y)", "x-y"},
      {"~(-1+x)", "-x"},
      {"~(9-x-y)", "-10+x+y"},
      {"~(-x-y)", "-1+x+y"},
      {"~(-1-2*x*y)", "2*x*y"},
      {"~(-1-x*(y|z))", "x*(y|z)"},
      {"~(-1-(x/y)-(z^w))", "x/y+(z^w)"}};
  for (unsigned Width : {3u, 8u, 32u, 64u, 128u, 257u}) {
    for (auto [Input, Expected] : Cases) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Input);
      SymContext Ctx;
      auto Actual = parseSymExpr(Ctx, Input, Width);
      auto Want = parseSymExpr(Ctx, Expected, Width);
      ASSERT_TRUE(Actual.ok());
      ASSERT_TRUE(Want.ok());
      EXPECT_EQ(Actual.Root, Want.Root) << Ctx.toString(Actual.Root);
    }
  }
}

TEST(SymExprComplement, RetainsOneBitFlagStructure) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 1), Y = Ctx.mkVar("y", 1);
  SymRef Sum = Ctx.mkAdd({Ctx.mkOne(1), X, Y});
  SymRef Result = Ctx.mkNot(Sum);
  EXPECT_EQ(Ctx.op(Result), SymOp::Not);
  EXPECT_EQ(Ctx.operand(Result, 0), Sum);
  for (uint64_t XV = 0; XV != 2; ++XV)
    for (uint64_t YV = 0; YV != 2; ++YV) {
      const std::array<uint64_t, 2> Values{XV, YV};
      EXPECT_EQ(Ctx.evalU64(Result, Values), (XV + YV) & 1);
    }
}

TEST(SymExprComplement, RetainsEqualCostAndMoreExpensiveForms) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 64), Y = Ctx.mkVar("y", 64);
  for (SymRef Sum : {Ctx.mkAdd(X, Y), Ctx.mkAdd(X, Ctx.mkOne(64)),
                     Ctx.mkAdd({Ctx.mkOnes(64), X, Y})}) {
    const size_t Before = Ctx.numNodes();
    SymRef Result = Ctx.mkNot(Sum);
    EXPECT_EQ(Ctx.op(Result), SymOp::Not);
    EXPECT_EQ(Ctx.operand(Result, 0), Sum);
    EXPECT_EQ(Ctx.numNodes(), Before + 1);
  }
}

TEST(SymExprComplement, ExhaustivelyPreservesModularAffineValues) {
  constexpr unsigned Width = 3;
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
  for (unsigned Offset = 0; Offset != 8; ++Offset)
    for (unsigned A = 0; A != 8; ++A)
      for (unsigned B = 0; B != 8; ++B) {
        SymRef Sum = Ctx.mkAdd({Ctx.mkConst(Width, Offset),
                                Ctx.mkMul(Ctx.mkConst(Width, A), X),
                                Ctx.mkMul(Ctx.mkConst(Width, B), Y)});
        SymRef Result = Ctx.mkNot(Sum);
        for (uint64_t XV = 0; XV != 8; ++XV)
          for (uint64_t YV = 0; YV != 8; ++YV) {
            const std::array<uint64_t, 2> Values{XV, YV};
            EXPECT_EQ(Ctx.evalU64(Result, Values),
                      (~(Offset + A * XV + B * YV)) & 7)
                << Offset << ',' << A << ',' << B << ',' << XV << ',' << YV;
          }
        EXPECT_LE(Ctx.readabilityCost(Result), Ctx.readabilityCost(Sum) + 1);
      }
}

TEST(SymExprComplement, PreservesSharedSourcesAndWidthAdapters) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 128), Y = Ctx.mkVar("y", 128);
  SymRef Source = Ctx.mkXor(X, Y);
  SymRef Sum = Ctx.mkAdd({Ctx.mkOnes(128), Ctx.mkNeg(Source), Ctx.mkNeg(X)});
  SymRef Wide = Ctx.mkNot(Sum);
  EXPECT_EQ(Wide, Ctx.mkAdd(Source, X));
  EXPECT_EQ(Ctx.op(Source), SymOp::Xor);
  SymRef Narrow = Ctx.mkExtract(Sum, 0, 8);
  EXPECT_EQ(Ctx.op(Ctx.mkNot(Narrow)), SymOp::Not);
}

TEST(SymExprComplement, KeepsDeepPositiveTailsLinearInStorage) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 64), One = Ctx.mkOne(64), Tail = X;
  const size_t Before = Ctx.numNodes();
  for (unsigned I = 0; I != 4096; ++I)
    Tail = Ctx.mkNot(Ctx.mkAdd(Tail, One));
  EXPECT_LE(Ctx.numNodes() - Before, 8192u);
  EXPECT_EQ(Ctx.op(Tail), SymOp::Not);
}
