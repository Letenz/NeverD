//===- SymMBAStructuralFactorTests.cpp - Whole factor recovery ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/symbolic/SymExpr.h"
#include "neverd/symbolic/SymMBA.h"

#include <array>

using namespace neverd::symbolic;

namespace {

SymRef distributed(SymContext &C, SymRef F, SymRef X, SymRef Y,
                   unsigned Offset = 0) {
  const unsigned W = C.width(F);
  return C.mkAdd(
      {C.mkMul(F, C.mkOr(X, Y)), C.mkMul(F, C.mkAnd(X, Y)),
       C.mkMul(F, C.mkNot(X)), C.mkNeg(C.mkMul(F, Y)),
       C.mkMul(C.mkConst(llvm::APInt(64, Offset).zextOrTrunc(W)), F)});
}

TEST(SymMBAStructuralFactor, RetainsWholeComplementBeforeChildRewrites) {
  SymContext C;
  SymRef U = C.mkVar("u", 8), X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  SymRef F = C.mkNot(U), Input = distributed(C, F, X, Y, 3);
  SymRef Expected = C.mkMul(C.mkConst(llvm::APInt(8, 2)), F);
  for (bool Deep : {false, true}) {
    MBAOptions Options;
    Options.VerifySamples = 0;
    auto Result = Deep ? simplifyMBADeep(C, Input, Options)
                       : simplifyMBA(C, Input, Options);
    EXPECT_LE(Result.SizeAfter, C.readabilityCost(Expected))
        << C.toString(Result.Expr);
    EXPECT_LE(Result.Work, Options.MaxWork);
  }
}

TEST(SymMBAStructuralFactor, HandlesWidthsScalesAndOpaqueSources) {
  for (unsigned W : {1u, 3u, 8u, 32u, 64u, 128u, 257u}) {
    SymContext C;
    SymRef U = C.mkVar("u", W), X = C.mkVar("x", W), Y = C.mkVar("y", W);
    for (SymRef F : {U, C.mkNot(U), C.mkUDiv(U, X)})
      for (unsigned Offset : {0u, 2u, 3u, 6u}) {
        SCOPED_TRACE(W);
        SCOPED_TRACE(Offset);
        SymRef Input = distributed(C, F, X, Y, Offset);
        SymRef Expected =
            C.mkMul(C.mkConst(llvm::APInt(64, Offset).zextOrTrunc(W) - 1), F);
        for (bool Deep : {false, true}) {
          MBAOptions Options;
          Options.VerifySamples = 0;
          auto Result = Deep ? simplifyMBADeep(C, Input, Options)
                             : simplifyMBA(C, Input, Options);
          EXPECT_LE(Result.SizeAfter, C.readabilityCost(Expected))
              << C.toString(Result.Expr);
          EXPECT_LE(Result.Work, Options.MaxWork);
          if (W <= 3)
            for (unsigned A = 0; A != (1u << W); ++A)
              for (unsigned B = 0; B != (1u << W); ++B)
                for (unsigned D = 0; D != (1u << W); ++D) {
                  std::array<uint64_t, 3> Values{A, B, D};
                  EXPECT_EQ(C.evalU64(Input, Values),
                            C.evalU64(Expected, Values));
                  EXPECT_EQ(C.evalU64(Input, Values),
                            C.evalU64(Result.Expr, Values));
                }
        }
      }
  }
}

TEST(SymMBAStructuralFactor, RemovesOnlyOneCopyOfARepeatedFactor) {
  for (unsigned W : {1u, 3u, 8u, 64u, 257u}) {
    SymContext C;
    SymRef X = C.mkVar("x", W), Y = C.mkVar("y", W), F = C.mkNot(X);
    SymRef Input = distributed(C, F, F, Y, 3);
    MBAOptions Options;
    Options.VerifySamples = 0;
    auto Result = simplifyMBADeep(C, Input, Options);
    SymRef Expected = C.mkMul(C.mkConst(llvm::APInt(64, 2).zextOrTrunc(W)), F);
    EXPECT_LE(Result.SizeAfter, C.readabilityCost(Expected));
    for (unsigned A = 0; A != 8; ++A)
      for (unsigned B = 0; B != 8; ++B) {
        std::array<llvm::APInt, 2> Values{llvm::APInt(64, A).zextOrTrunc(W),
                                          llvm::APInt(64, B).zextOrTrunc(W)};
        EXPECT_EQ(C.eval(Input, Values), C.eval(Result.Expr, Values));
        EXPECT_EQ(C.eval(Input, Values), C.eval(Expected, Values));
      }
  }
}

TEST(SymMBAStructuralFactor, LeavesUnmatchedTermsInTheirOriginalPartition) {
  SymContext C;
  constexpr unsigned W = 3;
  SymRef U = C.mkVar("u", W), X = C.mkVar("x", W), Y = C.mkVar("y", W);
  SymRef F = C.mkNot(U), Different = C.mkAdd(F, C.mkOne(W));
  SymRef Input = C.mkAdd({C.mkMul(F, C.mkOr(X, Y)), C.mkMul(F, C.mkAnd(X, Y)),
                          C.mkMul(Different, C.mkNot(X)),
                          C.mkNeg(C.mkMul(F, Y)), C.mkOne(W)});
  MBAOptions Options;
  Options.VerifySamples = 0;
  auto Result = simplifyMBADeep(C, Input, Options);
  EXPECT_LE(Result.SizeAfter, Result.SizeBefore);
  bool Differs = false;
  for (unsigned A = 0; A != 8; ++A)
    for (unsigned B = 0; B != 8; ++B)
      for (unsigned D = 0; D != 8; ++D) {
        std::array<uint64_t, 3> Values{A, B, D};
        const uint64_t Before = C.evalU64(Input, Values);
        EXPECT_EQ(Before, C.evalU64(Result.Expr, Values));
        Differs |= Before != C.evalU64(C.mkAdd(C.mkNeg(F), C.mkOne(W)), Values);
      }
  EXPECT_TRUE(Differs);
}

TEST(SymMBAStructuralFactor, RefusesResourcesAndRetainsCompletedCandidates) {
  SymContext C;
  SymRef U = C.mkVar("u", 3), V = C.mkVar("v", 3);
  SymRef X = C.mkVar("x", 3), Y = C.mkVar("y", 3);
  SymRef Input = C.mkAdd(distributed(C, C.mkNot(U), X, Y, 3),
                         distributed(C, C.mkNot(V), X, Y, 6));
  bool KeptBeforeRefusal = false;
  for (size_t Work : {size_t(0), size_t(1), size_t(32), size_t(256),
                      size_t(512), size_t(1024), size_t(2048), size_t(4096)}) {
    for (size_t Bytes :
         {size_t(0), size_t(1024), size_t(16384), size_t(1) << 22}) {
      MBAOptions Options;
      Options.MaxWork = Work;
      Options.MaxTableBytes = Bytes;
      Options.VerifySamples = 0;
      detail::WorkBudget Budget(Work);
      detail::SolveReport Report;
      const size_t Nodes = C.numNodes();
      SymRef Result =
          detail::solveStructuralFactors(C, Input, Options, Budget, Report);
      EXPECT_LE(Budget.used(), Work);
      EXPECT_LE(C.readabilityCost(Result), C.readabilityCost(Input));
      if (Work == 0 || Bytes == 0) {
        EXPECT_EQ(Result, Input);
        EXPECT_EQ(C.numNodes(), Nodes);
        EXPECT_TRUE(Report.BudgetExhausted);
      }
      KeptBeforeRefusal |= Result != Input && Report.BudgetExhausted;
      for (unsigned I = 0; I != 64; ++I) {
        std::array<uint64_t, 4> Values{I & 7u, (I >> 3) & 7u, (I * 3) & 7u,
                                       (I * 5) & 7u};
        EXPECT_EQ(C.evalU64(Input, Values), C.evalU64(Result, Values));
      }
    }
  }
  EXPECT_TRUE(KeptBeforeRefusal);
}

TEST(SymMBAStructuralFactor, KeepsSharedTailAndRefusalScansBounded) {
  size_t PreviousWork = 0;
  for (unsigned Depth : {0u, 2048u}) {
    SymContext C;
    SymRef U = C.mkVar("u", 64), X = C.mkVar("x", 64);
    SymRef Y = C.mkVar("y", 64), F = C.mkUDiv(U, Y);
    for (unsigned I = 0; I != Depth; ++I)
      F = C.mkUDiv(C.mkAdd(F, C.mkOne(64)), Y);
    SymRef Input = distributed(C, F, X, Y, 3);
    MBAOptions Options;
    Options.VerifySamples = 0;
    detail::WorkBudget Budget(4096);
    detail::SolveReport Report;
    SymRef Result =
        detail::solveStructuralFactors(C, Input, Options, Budget, Report);
    EXPECT_EQ(Result, C.mkMul(C.mkConst(llvm::APInt(64, 2)), F));
    EXPECT_FALSE(Report.BudgetExhausted);
    if (Depth)
      EXPECT_EQ(Budget.used(), PreviousWork);
    PreviousWork = Budget.used();

    llvm::SmallVector<SymRef, 65> Terms;
    for (unsigned I = 0; I != 65; ++I)
      Terms.push_back(
          C.mkMul(F, C.mkNot(C.mkVar("a" + std::to_string(I), 64))));
    SymRef Wide = C.mkAdd(Terms);
    detail::WorkBudget Small(32);
    detail::SolveReport Refused;
    const size_t Nodes = C.numNodes();
    EXPECT_EQ(detail::solveStructuralFactors(C, Wide, Options, Small, Refused),
              Wide);
    EXPECT_EQ(Small.used(), 0u);
    EXPECT_EQ(C.numNodes(), Nodes);
  }
}

} // namespace
