//===- SymMBACandidateSelectionTests.cpp - Layered candidate selection ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymExpr.h"
#include "neverd/symbolic/SymMBA.h"
#include "neverd/symbolic/SymParse.h"

#include <array>

using namespace neverd::symbolic;

namespace {

TEST(SymMBACandidateSelection, RefinesNewInternalLinearRegions) {
  for (unsigned Width : {3u, 8u, 32u, 64u, 257u}) {
    for (const auto &[Text, Expected] :
         {std::pair{"z*(x^y)+2*z*(x&y)", "z*(x+y)"},
          std::pair{"x*(y^z)+2*x*(y&z)+x*w", "x*(y+z+w)"}}) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Text);
      SymContext C;
      auto Input = parseSymExpr(C, Text, Width);
      auto Want = parseSymExpr(C, Expected, Width);
      ASSERT_TRUE(Input.ok());
      ASSERT_TRUE(Want.ok());
      MBAOptions Options;
      Options.VerifySamples = 0;
      auto Result = simplifyMBADeep(C, Input.Root, Options);
      EXPECT_EQ(Result.Expr, Want.Root) << C.toString(Result.Expr);
      EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
      EXPECT_LE(Result.Work, Options.MaxWork);
    }
  }
}

TEST(SymMBACandidateSelection, KeepsWholeRegionRelationsAcrossChildRewrites) {
  for (unsigned Width : {3u, 8u, 32u, 64u, 257u}) {
    SymContext C;
    auto Input = parseSymExpr(C, "3*x-2*(x&y)-z+(~(3*x-2*(x&y))|z)", Width);
    auto Want = parseSymExpr(C, "(3*x-2*(x&y))|~z", Width);
    ASSERT_TRUE(Input.ok());
    ASSERT_TRUE(Want.ok());
    MBAOptions Options;
    Options.VerifySamples = 0;
    auto Result = simplifyMBADeep(C, Input.Root, Options);
    EXPECT_LE(Result.SizeAfter, C.readabilityCost(Want.Root))
        << C.toString(Result.Expr);
    EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
    EXPECT_LE(Result.Work, Options.MaxWork);
  }
}

TEST(SymMBACandidateSelection, PreservesNearbyFormsExhaustively) {
  for (const char *Text :
       {"z*(x^y)+3*z*(x&y)", "3*x-2*(x&y)-z+(~(3*x-3*(x&y))|z)",
        "3*x-2*(x&y)-z+(~(3*x-2*(x&y))&z)"}) {
    SymContext C;
    auto Input = parseSymExpr(C, Text, 3);
    ASSERT_TRUE(Input.ok());
    MBAOptions Options;
    Options.VerifySamples = 0;
    auto Result = simplifyMBADeep(C, Input.Root, Options);
    for (uint64_t X = 0; X != 8; ++X)
      for (uint64_t Y = 0; Y != 8; ++Y)
        for (uint64_t Z = 0; Z != 8; ++Z) {
          std::array<uint64_t, 3> Values{X, Y, Z};
          EXPECT_EQ(C.evalU64(Input.Root, Values),
                    C.evalU64(Result.Expr, Values))
              << Text;
        }
    EXPECT_LE(Result.SizeAfter, Result.SizeBefore);
  }
}

TEST(SymMBACandidateSelection, SharesResourceLimitsAcrossBothCandidates) {
  for (unsigned Width : {1u, 3u, 64u, 257u})
    for (size_t Work :
         {size_t(0), size_t(1), size_t(64), size_t(1024), size_t(4096)})
      for (size_t Bytes : {size_t(0), size_t(128), size_t(4096)}) {
        SymContext C;
        SymRef X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
        SymRef Input =
            C.mkAdd(C.mkMul(X, C.mkXor(X, Y)),
                    C.mkMul(C.mkConst(Width, 2), C.mkMul(X, C.mkAnd(X, Y))));
        MBAOptions Options;
        Options.MaxWork = Work;
        Options.MaxTableBytes = Bytes;
        Options.VerifySamples = 0;
        auto Result = simplifyMBADeep(C, Input, Options);
        EXPECT_LE(Result.Work, Work);
        EXPECT_LE(Result.SizeAfter, Result.SizeBefore);
        if (!Work)
          EXPECT_EQ(Result.Expr, Input);
        for (uint64_t A : {uint64_t(0), uint64_t(1), uint64_t(7), UINT64_MAX})
          for (uint64_t B :
               {uint64_t(0), uint64_t(2), uint64_t(5), UINT64_MAX}) {
            std::array<llvm::APInt, 2> Values{
                llvm::APInt(64, A).zextOrTrunc(Width),
                llvm::APInt(64, B).zextOrTrunc(Width)};
            EXPECT_EQ(C.eval(Input, Values), C.eval(Result.Expr, Values));
          }
      }
}

TEST(SymMBACandidateSelection, ReusesSharedGeneratedRegionsBehindOpaqueUses) {
  for (unsigned Count : {4u, 16u, 64u}) {
    SymContext C;
    SymRef X = C.mkVar("x", 64), Y = C.mkVar("y", 64);
    SymRef Z = C.mkVar("z", 64);
    SymRef Input =
        C.mkAdd(C.mkMul(Z, C.mkXor(X, Y)),
                C.mkMul(C.mkConst(64, 2), C.mkMul(Z, C.mkAnd(X, Y))));
    SymRef Want = C.mkMul(Z, C.mkAdd(X, Y));
    SymRef Uses = C.mkConst(64, 3), Expected = Uses;
    for (unsigned I = 0; I != Count; ++I) {
      SymRef D = C.mkVar("d" + std::to_string(I), 64);
      Uses = C.mkUDiv(Uses, C.mkUDiv(Input, D));
      Expected = C.mkUDiv(Expected, C.mkUDiv(Want, D));
    }
    MBAOptions Options;
    Options.VerifySamples = 0;
    auto Result = simplifyMBADeep(C, Uses, Options);
    EXPECT_EQ(Result.Expr, Expected);
    EXPECT_LE(Result.Work, 2000u + 100u * Count);
  }
}

TEST(SymMBACandidateSelection, AvoidsRepeatedWholeReadingsOfProductFactors) {
  SymContext C;
  auto Input = parseSymExpr(C, "(x+~y)*(z^w)+(x+~y)*(a|b)+(x+~y)", 64);
  ASSERT_TRUE(Input.ok());
  MBAOptions Options;
  Options.VerifySamples = 0;
  auto Result = simplifyMBADeep(C, Input.Root, Options);
  EXPECT_TRUE(Result.Changed);
  EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
  EXPECT_LE(Result.Work, 3300u);
  EXPECT_LE(Result.SizeAfter, Result.SizeBefore);
}

} // namespace
