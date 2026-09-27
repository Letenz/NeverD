//===- SymMBASelectorTests.cpp - Linear weight selector candidates --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymExpr.h"
#include "neverd/symbolic/SymMBA.h"
#include "neverd/symbolic/SymParse.h"

#include <cstdint>
#include <string>

using namespace neverd::symbolic;

namespace {

void simplifiesTo(llvm::StringRef Input, llvm::StringRef Expected,
                  uint32_t Width) {
  SCOPED_TRACE(Width);
  SCOPED_TRACE(Input.str());
  SymContext Ctx;
  SymParseResult Original = parseSymExpr(Ctx, Input, Width);
  SymParseResult Want = parseSymExpr(Ctx, Expected, Width);
  ASSERT_TRUE(Original.ok()) << Original.Error;
  ASSERT_TRUE(Want.ok()) << Want.Error;
  MBAOptions Opts;
  Opts.VerifySamples = 0;
  MBAResult R = simplifyMBA(Ctx, Original.Root, Opts);
  EXPECT_EQ(R.Expr, Want.Root) << Ctx.toString(R.Expr);
  EXPECT_LE(R.SizeAfter, R.SizeBefore);
  EXPECT_LE(R.Work, Opts.MaxWork);
  if (R.Changed)
    EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
}

TEST(SymMBASelector, KeepsIndependentWeightedAndFactoredVariableSums) {
  for (unsigned Count : {4u, 6u, 8u}) {
    std::string Weighted;
    std::string Sum;
    for (unsigned I = 0; I < Count; ++I) {
      if (I != 0) {
        Weighted += " + ";
        Sum += " + ";
      }
      const std::string Variable = "x" + std::to_string(I);
      Weighted += std::to_string(I % 3 + 2) + " * " + Variable;
      Sum += Variable;
    }
    const std::string Factored = "2 * (" + Sum + ")";
    for (uint32_t Width : {3u, 8u, 32u, 64u, 129u, 256u}) {
      simplifiesTo(Weighted, Weighted, Width);
      simplifiesTo(Factored, Factored, Width);
    }
  }
}

TEST(SymMBASelector, KeepsSignedAndWideIndependentCoefficients) {
  constexpr llvm::StringLiteral Signed(
      "-2 * x0 + 3 * x1 - 4 * x2 + 2 * x3 - 3 * x4 + 4 * x5");
  for (uint32_t Width : {3u, 8u, 32u, 64u, 129u, 256u})
    simplifiesTo(Signed, Signed, Width);

  constexpr llvm::StringLiteral Wide(
      "0x100000000000000000000000000000002 * x0 + 3 * x1 - 2 * x2");
  for (uint32_t Width : {129u, 256u})
    simplifiesTo(Wide, Wide, Width);
}

TEST(SymMBASelector, StillRecoversOverlappingSelectors) {
  // Each expression has an interaction between inputs.  In particular, its
  // zero-weight minterm must not be added to the cumulative selector support.
  for (uint32_t Width : {3u, 8u, 32u, 64u, 129u, 256u}) {
    simplifiesTo("2 * x + (~x & y)", "x + (x | y)", Width);
    simplifiesTo("3 * x + (~x & y)", "2 * x + (x | y)", Width);
    simplifiesTo("-2 * x - (~x & y)", "-x - (x | y)", Width);
  }
}

TEST(SymMBASelector, RetainsEstablishedCandidatesForIndependentWeights) {
  // Skipping cumulative selectors must leave the older grouped and
  // conjunction candidates available to recover an ordinary variable sum.
  for (uint32_t Width : {3u, 32u, 64u, 256u}) {
    simplifiesTo("(x ^ y) + 2 * (x & y)", "x + y", Width);
    simplifiesTo("2 * (x | y) - (x ^ y)", "x + y", Width);
    simplifiesTo("(x | y) - (x & y)", "x ^ y", Width);
  }
}

} // namespace
