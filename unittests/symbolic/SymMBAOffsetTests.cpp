//===- SymMBAOffsetTests.cpp - Independent sums with a shared offset ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymMBA.h"
#include "neverd/symbolic/SymParse.h"

using namespace neverd::symbolic;

namespace {

TEST(SymMBAOffset, IncludesTheOffsetInOneIndependentRegion) {
  MBAOptions Options;
  Options.VerifySamples = 0;
  for (unsigned W : {3u, 8u, 32u, 64u, 128u, 256u}) {
    SCOPED_TRACE(W);
    SymContext C;
    auto Input = parseSymExpr(C, "2*a - p + (e & p) - 1", W);
    auto Expected = parseSymExpr(C, "2*a + (e | ~p)", W);
    ASSERT_TRUE(Input.ok());
    ASSERT_TRUE(Expected.ok());
    MBAResult Result = simplifyMBA(C, Input.Root, Options);
    EXPECT_EQ(Result.Expr, Expected.Root) << C.toString(Result.Expr);
    EXPECT_LT(Result.SizeAfter, Result.SizeBefore);
    EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
  }
}

TEST(SymMBAOffset, KeepsTheOffsetExactlyOnceAcrossSeveralGroups) {
  SymContext C;
  constexpr unsigned W = 3;
  auto Input = parseSymExpr(C, "-1 - p + (e & p) - q + (f & q)", W);
  ASSERT_TRUE(Input.ok());
  MBAOptions Options;
  Options.VerifySamples = 0;
  MBAResult Result = simplifyMBA(C, Input.Root, Options);
  EXPECT_LT(Result.SizeAfter, Result.SizeBefore);
  for (unsigned P = 0; P < 8; ++P)
    for (unsigned E = 0; E < 8; ++E)
      for (unsigned Q = 0; Q < 8; ++Q)
        for (unsigned F = 0; F < 8; ++F) {
          llvm::APInt PV(W, P), EV(W, E), QV(W, Q), FV(W, F);
          EXPECT_EQ(C.eval(Result.Expr, {PV, EV, QV, FV}),
                    llvm::APInt::getAllOnes(W) - PV + (EV & PV) - QV +
                        (FV & QV));
        }
}

TEST(SymMBAOffset, NearbyOffsetsAndProductsKeepTheirFullMeaning) {
  MBAOptions Options;
  Options.VerifySamples = 0;
  for (unsigned W : {3u, 8u, 64u, 256u}) {
    SymContext C;
    auto Input = parseSymExpr(C, "2*a - p + (e & p) - 2", W);
    auto Wrong = parseSymExpr(C, "2*a + (e | ~p)", W);
    ASSERT_TRUE(Input.ok());
    ASSERT_TRUE(Wrong.ok());
    MBAResult Result = simplifyMBA(C, Input.Root, Options);
    EXPECT_NE(Result.Expr, Wrong.Root);
    for (unsigned A : {0u, 1u, 5u})
      for (unsigned P : {0u, 1u, 7u})
        for (unsigned E : {0u, 3u, 7u}) {
          llvm::APInt AV(W, A), PV(W, P), EV(W, E);
          EXPECT_EQ(C.eval(Result.Expr, {AV, PV, EV}),
                    2 * AV - PV + (EV & PV) - 2);
        }
  }
}

TEST(SymMBAOffset, RefusesOptionalWorkWhenBudgetsRunOut) {
  for (size_t Limit : {size_t(0), size_t(1), size_t(16), size_t(64)}) {
    SymContext C;
    auto Input = parseSymExpr(C, "2*a - p + (e & p) - 1", 32);
    ASSERT_TRUE(Input.ok());
    MBAOptions Options;
    Options.VerifySamples = 0;
    Options.MaxWork = Limit;
    MBAResult Result = simplifyMBA(C, Input.Root, Options);
    EXPECT_LE(Result.Work, Limit);
    EXPECT_EQ(C.evalU64(Result.Expr, {5, 13, 7}),
              uint32_t(2 * 5 - 13 + (7 & 13) - 1));
  }

  SymContext C;
  auto Input = parseSymExpr(C, "2*a - p + (e & p) - 1", 32);
  ASSERT_TRUE(Input.ok());
  MBAOptions Options;
  Options.VerifySamples = 0;
  Options.MaxTableBytes = 0;
  EXPECT_EQ(simplifyMBA(C, Input.Root, Options).Expr, Input.Root);
}

TEST(SymMBAOffset, ASharedOffsetDoesNotRequireEnumeratingEveryGroup) {
  for (unsigned W : {8u, 64u, 256u}) {
    for (unsigned Count : {2u, 8u, 32u}) {
      SCOPED_TRACE(W);
      SCOPED_TRACE(Count);
      SymContext C;
      llvm::SmallVector<SymRef, 32> Terms{C.mkOnes(W)};
      llvm::SmallVector<llvm::APInt, 64> Values;
      llvm::APInt Expected = llvm::APInt::getAllOnes(W);
      for (unsigned I = 0; I < Count; ++I) {
        SymRef P = C.mkVar("p" + std::to_string(I), W);
        SymRef E = C.mkVar("e" + std::to_string(I), W);
        Terms.push_back(C.mkSub(C.mkAnd(P, E), P));
        llvm::APInt PV(W, I + 1), EV(W, 3 * I + 1);
        Values.push_back(PV);
        Values.push_back(EV);
        Expected += (PV & EV) - PV;
      }
      SymRef Input = C.mkAdd(Terms);
      MBAOptions Options;
      Options.VerifySamples = 0;
      Options.MaxWork = 100000;
      MBAResult Result = simplifyMBA(C, Input, Options);
      EXPECT_LT(Result.SizeAfter, Result.SizeBefore);
      EXPECT_LE(Result.Work, Options.MaxWork);
      EXPECT_EQ(C.eval(Result.Expr, Values), Expected);
      llvm::SmallVector<uint32_t, 64> Variables;
      C.collectVars(Result.Expr, Variables);
      for (uint32_t Id : Variables)
        EXPECT_LT(Id, 2 * Count);
    }
  }
}

} // namespace
