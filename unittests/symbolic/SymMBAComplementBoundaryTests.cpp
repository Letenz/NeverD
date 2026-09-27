//===- SymMBAComplementBoundaryTests.cpp - Compound complement inputs -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymExpr.h"
#include "neverd/symbolic/SymMBA.h"

#include <array>

using namespace neverd::symbolic;

namespace {

SymRef splitProduct(SymContext &C, SymRef X, SymRef Y) {
  return C.mkAdd(C.mkMul(C.mkAnd(X, Y), C.mkOr(X, Y)),
                 C.mkMul(C.mkAnd(X, C.mkNot(Y)), C.mkAnd(C.mkNot(X), Y)));
}

TEST(SymMBAComplementBoundary, RetainsCompoundSourcesForProductRecovery) {
  for (unsigned Width : {1u, 3u, 8u, 32u, 64u, 128u, 257u}) {
    SymContext C;
    SymRef X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    SymRef Z = C.mkVar("z", Width), One = C.mkOne(Width);
    SymRef Three = C.mkConst(llvm::APInt(64, 3).zextOrTrunc(Width));
    SymRef Quadratic = C.mkSub(C.mkMul(Y, Y), Y);
    SymRef Cubic = C.mkSub(C.mkMul({Y, Y, Y}), C.mkMul(Three, Y));
    for (auto [Input, Expected] :
         {std::pair{splitProduct(C, X, Quadratic),
                    C.mkMul({X, Y, C.mkSub(Y, One)})},
          std::pair{splitProduct(C, X, Cubic),
                    C.mkMul({X, Y, C.mkSub(C.mkMul(Y, Y), Three)})},
          std::pair{splitProduct(
                        C, X, splitProduct(C, Y, C.mkMul(Z, C.mkSub(Z, One)))),
                    C.mkMul({X, Y, Z, C.mkSub(Z, One)})}}) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(C.toString(Input));
      MBAOptions Options;
      Options.VerifySamples = 0;
      auto Result = simplifyMBADeep(C, Input, Options);
      // One-bit inputs retain their flag topology; their values are checked
      // exhaustively below without imposing a word-level factor spelling.
      if (Width > 1)
        EXPECT_LE(Result.SizeAfter, C.readabilityCost(Expected))
            << C.toString(Result.Expr);
      EXPECT_LE(Result.Work, Options.MaxWork);
      EXPECT_LE(Result.SizeAfter, Result.SizeBefore);
      if (Width <= 3)
        for (uint64_t A = 0; A != (1u << Width); ++A)
          for (uint64_t B = 0; B != (1u << Width); ++B)
            for (uint64_t D = 0; D != (1u << Width); ++D) {
              std::array<uint64_t, 3> Values{A, B, D};
              EXPECT_EQ(C.evalU64(Input, Values),
                        C.evalU64(Result.Expr, Values));
              EXPECT_EQ(C.evalU64(Input, Values), C.evalU64(Expected, Values));
            }
    }
  }
}

TEST(SymMBAComplementBoundary, PreservesNearbyProductFormsAndBudgetRefusal) {
  constexpr unsigned Width = 3;
  SymContext C;
  SymRef X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
  SymRef Source = C.mkSub(C.mkMul(Y, Y), Y);
  SymRef Different = C.mkAdd(Source, C.mkOne(Width));
  SymRef Input = C.mkAdd(
      C.mkMul(C.mkAnd(X, Source), C.mkOr(X, Source)),
      C.mkMul(C.mkAnd(X, C.mkNot(Different)), C.mkAnd(C.mkNot(X), Source)));
  for (size_t Limit : {size_t(0), size_t(1), size_t(64), size_t(4096)}) {
    MBAOptions Options;
    Options.MaxWork = Limit;
    Options.VerifySamples = 0;
    auto Result = simplifyMBADeep(C, Input, Options);
    EXPECT_LE(Result.Work, Limit);
    EXPECT_LE(Result.SizeAfter, Result.SizeBefore);
    if (!Limit)
      EXPECT_EQ(Result.Expr, Input);
    bool DiffersFromProduct = false;
    for (uint64_t A = 0; A != 8; ++A)
      for (uint64_t B = 0; B != 8; ++B) {
        std::array<uint64_t, 2> Values{A, B};
        EXPECT_EQ(C.evalU64(Input, Values), C.evalU64(Result.Expr, Values));
        DiffersFromProduct |=
            C.evalU64(Input, Values) != C.evalU64(C.mkMul(X, Source), Values);
      }
    EXPECT_TRUE(DiffersFromProduct);
  }
}

} // namespace
