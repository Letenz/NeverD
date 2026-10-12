//===- SymMBAMinimumTests.cpp - Proven expression minima ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Checks that ordinary expressions stop at a proved reading bound while
/// related arithmetic inputs still receive the complete MBA analysis.
///
//===----------------------------------------------------------------------===//

#include "SymMBATestsDetail.h"
#include "gtest/gtest.h"

#include <utility>
#include <vector>

using namespace neverd::symbolic;

namespace {

TEST(SymMBA, ProvesBinaryLeafMinimaWithoutSearching) {
  for (unsigned Width : {2u, 3u, 8u, 32u, 64u, 128u, 257u}) {
    for (const char *Text :
         {"x + 1", "x + 2", "x * 2", "x & 1", "x | 1", "x ^ 1", "x + y",
          "x * y", "x & y", "x | y", "x ^ y"}) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Text);
      SymContext Ctx;
      SymParseResult P = parseSymExpr(Ctx, Text, Width);
      ASSERT_TRUE(P.ok()) << P.Error;
      ASSERT_EQ(Ctx.readabilityCost(P.Root), 3u);
      MBAOptions Tight;
      // Enough to visit this expression, not enough for corner measurement
      // and synthesis. Raising the budget cannot find a cheaper spelling.
      Tight.MaxWork = 3;
      Tight.VerifySamples = 0;
      for (bool Deep : {false, true}) {
        MBAResult R = Deep ? simplifyMBADeep(Ctx, P.Root, Tight)
                           : simplifyMBA(Ctx, P.Root, Tight);
        EXPECT_EQ(R.Expr, P.Root);
        EXPECT_FALSE(R.Changed);
        EXPECT_EQ(R.Outcome, MBAOutcome::AlreadyShortest);
        EXPECT_EQ(R.Evidence, MBAEvidence::None);
        EXPECT_LE(R.Work, Tight.MaxWork);
      }
    }
  }
}

TEST(SymMBA, BinaryLeafBoundsExcludeEveryCheaperSmallWordFunction) {
  for (unsigned Width : {2u, 3u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    const unsigned Limit = 1u << Width;
    std::vector<SymRef> Cheaper{
        X, Y, Ctx.mkNot(X), Ctx.mkNot(Y), Ctx.mkNeg(X), Ctx.mkNeg(Y)};
    for (unsigned C = 0; C < Limit; ++C)
      Cheaper.push_back(Ctx.mkConst(Width, C));
    auto values = [&](SymRef E) {
      std::vector<uint64_t> Out, Inputs(Ctx.numVars());
      for (unsigned A = 0; A < Limit; ++A) {
        for (unsigned B = 0; B < Limit; ++B) {
          Inputs[Ctx.varId(X)] = A;
          Inputs[Ctx.varId(Y)] = B;
          Out.push_back(Ctx.evalU64(E, Inputs));
        }
      }
      return Out;
    };
    std::vector<std::vector<uint64_t>> CheaperValues;
    for (SymRef E : Cheaper)
      CheaperValues.push_back(values(E));
    for (unsigned C = 0; C <= Limit; ++C) {
      SymRef Other = C == Limit ? Y : Ctx.mkConst(Width, C);
      for (SymRef E :
           {Ctx.mkAdd(X, Other), Ctx.mkMul(X, Other), Ctx.mkAnd(X, Other),
            Ctx.mkOr(X, Other), Ctx.mkXor(X, Other)}) {
        // Constructors already fold zero, identity and unary-sign cases.
        if (Ctx.readabilityCost(E) != 3)
          continue;
        SCOPED_TRACE(Ctx.toString(E));
        const auto Expected = values(E);
        for (const auto &Alternative : CheaperValues)
          EXPECT_NE(Expected, Alternative);
        MBAOptions Tight;
        Tight.MaxWork = 3;
        MBAResult R = simplifyMBADeep(Ctx, E, Tight);
        EXPECT_EQ(R.Outcome, MBAOutcome::AlreadyShortest);
        EXPECT_EQ(values(R.Expr), Expected);
      }
    }
  }
}

TEST(SymMBA, BinaryLeafProofPreservesResourceRefusals) {
  SymContext Ctx;
  for (const char *Text : {"x + 1", "x ^ y"}) {
    SymParseResult P = parseSymExpr(Ctx, Text, 32);
    ASSERT_TRUE(P.ok());
    for (bool Deep : {false, true}) {
      for (unsigned Kind : {0u, 1u, 2u}) {
        MBAOptions Opts;
        if (Kind == 0)
          Opts.MaxWork = 0;
        else if (Kind == 1)
          Opts.MaxAtoms = 0;
        else
          Opts.MaxTableBytes = 0;
        MBAResult R = Deep ? simplifyMBADeep(Ctx, P.Root, Opts)
                           : simplifyMBA(Ctx, P.Root, Opts);
        EXPECT_EQ(R.Expr, P.Root);
        EXPECT_FALSE(R.Changed);
        EXPECT_EQ(R.Outcome, Kind == 0 ? MBAOutcome::BudgetExhausted
                                       : MBAOutcome::TooManyInputs);
      }
    }
  }
}

TEST(SymMBA, BinaryLeafProofDoesNotHideRelatedOrOpaqueInputs) {
  const std::pair<const char *, const char *> Cases[] = {
      {"(x ^ (x + 1)) + 2 * (x & (x + 1))", "2 * x + 1"},
      {"((x & 0x55) ^ y) + 2 * ((x & 0x55) & y)", "(x & 0x55) + y"},
      {"((x / y) ^ z) + 2 * ((x / y) & z)", "x / y + z"},
      {"x * y + x * z", "x * (y + z)"},
      {"~x + 1", "-x"},
      {"((x ^ y) + 2 * (x & y)) ^ ((x | y) + (x & y))", "0"},
  };
  for (unsigned Width : {8u, 32u, 257u}) {
    for (auto [Text, Expected] : Cases) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Text);
      SymContext Ctx;
      SymParseResult P = parseSymExpr(Ctx, Text, Width);
      SymParseResult Want = parseSymExpr(Ctx, Expected, Width);
      ASSERT_TRUE(P.ok());
      ASSERT_TRUE(Want.ok());
      MBAOptions ProofOnly;
      ProofOnly.VerifySamples = 0;
      MBAResult R = simplifyMBADeep(Ctx, P.Root, ProofOnly);
      EXPECT_EQ(R.Expr, Want.Root) << Ctx.toString(R.Expr);
      EXPECT_TRUE(R.Changed);
      EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
    }
  }
}

} // namespace
