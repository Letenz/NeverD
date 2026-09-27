//===- SymExprCompareTests.cpp - Subtraction predicate recovery -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/expr/SymExprCompare.h"
#include "gtest/gtest.h"

#include "neverd/symbolic/SymExpr.h"

using namespace neverd::symbolic;

namespace {

SymRef signedWord(SymContext &C, SymRef X, SymRef Y) {
  SymRef D = C.mkSub(X, Y);
  return C.mkXor(D, C.mkAnd(C.mkXor(X, Y), C.mkXor(X, D)));
}

SymRef borrowWord(SymContext &C, SymRef X, SymRef Y) {
  return C.mkOr(C.mkAnd(C.mkNot(X), Y),
                C.mkAnd(C.mkNot(C.mkXor(X, Y)), C.mkSub(X, Y)));
}

TEST(SymExprCompare, HighestBitRecoversSignedAndUnsignedComparisons) {
  for (unsigned W : {1u, 3u, 8u, 32u, 64u, 128u, 256u}) {
    SCOPED_TRACE(W);
    SymContext C;
    SymRef X = C.mkVar("x", W), Y = C.mkVar("y", W);
    SymRef S = signedWord(C, X, Y), U = borrowWord(C, X, Y);
    SCOPED_TRACE(C.toString(U));
    SymRef Count = C.mkConst(16, W - 1);
    EXPECT_EQ(C.mkLShr(S, Count), C.mkZExt(C.mkSlt(X, Y), W));
    EXPECT_EQ(C.mkLShr(U, Count), C.mkZExt(C.mkUlt(X, Y), W));
    EXPECT_EQ(C.mkExtract(S, W - 1, 1), C.mkSlt(X, Y));
    EXPECT_EQ(C.mkExtract(U, W - 1, 1), C.mkUlt(X, Y));
    EXPECT_EQ(C.mkAShr(S, Count), C.mkSExt(C.mkSlt(X, Y), W));
    EXPECT_EQ(C.mkAShr(U, C.mkConst(16, W + 1)), C.mkSExt(C.mkUlt(X, Y), W));
  }
}

TEST(SymExprCompare, SplitSignAndOverflowViewsShareTheSamePredicate) {
  for (unsigned W : {3u, 8u, 32u, 64u, 128u, 256u}) {
    SCOPED_TRACE(W);
    SymContext C;
    SymRef Y = C.mkVar("y", W), X = C.mkVar("x", W);
    SymRef D = C.mkSub(X, Y);
    SymRef V = C.mkAnd(C.mkXor(Y, X), C.mkXor(D, X));
    SymRef Count = C.mkConst(16, W - 1);
    SymRef Expected = C.mkSlt(X, Y);
    EXPECT_EQ(C.mkXor(C.mkLShr(D, Count), C.mkLShr(V, Count)),
              C.mkZExt(Expected, W));
    EXPECT_EQ(C.mkXor(C.mkExtract(V, W - 1, 1), C.mkExtract(D, W - 1, 1)),
              Expected);
    EXPECT_EQ(C.mkXor(C.mkSlt(V, C.mkZero(W)), C.mkSlt(D, C.mkZero(W))),
              Expected);
    EXPECT_EQ(C.mkXor(C.mkZExt(C.mkSlt(V, C.mkZero(W)), 8),
                      C.mkZExt(C.mkExtract(D, W - 1, 1), 8)),
              C.mkZExt(Expected, 8));
  }
}

TEST(SymExprCompare, WrongDifferenceAndProjectionRemainDistinct) {
  for (unsigned W : {3u, 8u, 32u, 64u, 128u, 256u}) {
    SymContext C;
    SymRef X = C.mkVar("x", W), Y = C.mkVar("y", W);
    SymRef Z = C.mkVar("z", W), D = C.mkSub(Z, Y);
    SymRef Wrong = C.mkXor(D, C.mkAnd(C.mkXor(X, Y), C.mkXor(X, D)));
    SymRef Expected = C.mkZExt(C.mkSlt(X, Y), W);
    EXPECT_NE(C.mkLShr(Wrong, C.mkConst(16, W - 1)), Expected);
    SymRef S = signedWord(C, X, Y);
    EXPECT_NE(C.mkLShr(S, C.mkConst(16, W - 2)), Expected);
    EXPECT_NE(C.mkLShr(S, C.mkVar("count", 16)), Expected);
    EXPECT_NE(C.mkLShr(S, C.mkConst(16, W - 1)), C.mkZExt(C.mkSlt(Y, X), W));
    EXPECT_EQ(C.mkLShr(S, C.mkConst(16, W)), C.mkZero(W));
  }
}

TEST(SymExprCompare, CanonicalDifferencesKeepCompoundOperands) {
  for (unsigned W : {1u, 3u, 8u, 32u, 128u, 256u}) {
    SymContext C;
    SymRef A = C.mkVar("a", W), B = C.mkVar("b", W);
    SymRef V = C.mkVar("v", W), Z = C.mkVar("z", W);
    for (SymRef X : {C.mkAdd(A, B), C.mkMul(A, B)}) {
      for (SymRef Y : {C.mkAdd(V, Z), C.mkNeg(C.mkAdd(V, Z)),
                       C.mkMul(C.mkConst(W, 3), V)}) {
        SCOPED_TRACE(W);
        SCOPED_TRACE(C.toString(X) + " vs " + C.toString(Y));
        SymRef S = signedWord(C, X, Y);
        SymRef U = borrowWord(C, X, Y);
        EXPECT_EQ(C.mkExtract(S, W - 1, 1), C.mkSlt(X, Y));
        EXPECT_EQ(C.mkExtract(U, W - 1, 1), C.mkUlt(X, Y));
      }
    }
  }
}

TEST(SymExprCompare, ExhaustiveSmallWordsKeepFullValueConsumers) {
  for (unsigned W : {1u, 3u, 8u}) {
    SymContext C;
    SymRef X = C.mkVar("x", W), Y = C.mkVar("y", W);
    SymRef D = C.mkSub(X, Y);
    SymRef V = C.mkAnd(C.mkXor(X, Y), C.mkXor(X, D));
    SymRef S = C.mkXor(D, V), U = borrowWord(C, X, Y);
    SymRef SP = C.mkExtract(S, W - 1, 1);
    SymRef UP = C.mkExtract(U, W - 1, 1);
    const unsigned Limit = 1u << W;
    for (unsigned A = 0; A < Limit; ++A) {
      for (unsigned B = 0; B < Limit; ++B) {
        llvm::APInt AV(W, A), BV(W, B), DV = AV - BV;
        llvm::SmallVector<llvm::APInt, 2> Values{AV, BV};
        EXPECT_EQ(C.eval(SP, Values), llvm::APInt(1, AV.slt(BV)));
        EXPECT_EQ(C.eval(UP, Values), llvm::APInt(1, AV.ult(BV)));
        EXPECT_EQ(C.eval(D, Values), DV);
        EXPECT_EQ(C.eval(V, Values), (AV ^ BV) & (AV ^ DV));
        EXPECT_EQ(C.eval(S, Values), DV ^ ((AV ^ BV) & (AV ^ DV)));
        EXPECT_EQ(C.eval(U, Values), (~AV & BV) | (~(AV ^ BV) & DV));
      }
    }
  }
}

TEST(SymExprCompare, DeepAndWideRefusalDoesNotInternOrRecurse) {
  SymContext C;
  SymRef X = C.mkVar("x", 32), Y = C.mkVar("y", 32);
  SymRef E = signedWord(C, X, Y);
  for (unsigned I = 0; I < 10000; ++I)
    E = C.mkXor(C.mkAnd(E, X), C.mkAnd(E, C.mkNot(X)));
  const size_t BeforeDeep = C.numNodes();
  EXPECT_FALSE(detail::recoverSignComparison(C, E));
  EXPECT_EQ(C.numNodes(), BeforeDeep);
  EXPECT_EQ(C.evalU64(E, {17, 23}),
            (uint32_t(17 - 23) ^ ((17 ^ 23) & (17 ^ uint32_t(17 - 23)))));
  llvm::SmallVector<SymRef, 80> Arms;
  for (unsigned I = 0; I < 80; ++I)
    Arms.push_back(C.mkVar("arm" + std::to_string(I), 32));
  SymRef Wide = C.mkOr(Arms);
  const size_t BeforeWide = C.numNodes();
  EXPECT_FALSE(detail::recoverSignComparison(C, Wide));
  EXPECT_EQ(C.numNodes(), BeforeWide);
}

TEST(SymExprCompare, ComplementAndBorrowXorFormsRecoverExactly) {
  for (unsigned W : {3u, 8u, 64u, 256u}) {
    SymContext C;
    SymRef X = C.mkVar("x", W), Y = C.mkVar("y", W);
    SymRef D = C.mkSub(X, Y);
    SymRef U = C.mkXor(D, C.mkAnd(C.mkXor(D, Y), C.mkXor(X, Y)));
    EXPECT_EQ(C.mkExtract(U, W - 1, 1), C.mkUlt(X, Y));
    EXPECT_EQ(C.mkExtract(C.mkNot(U), W - 1, 1), C.mkNot(C.mkUlt(X, Y)));
    SymRef S = signedWord(C, X, Y);
    EXPECT_EQ(C.mkSlt(S, C.mkZero(W)), C.mkSlt(X, Y));
    EXPECT_EQ(C.mkSlt(C.mkNot(S), C.mkZero(W)), C.mkNot(C.mkSlt(X, Y)));
  }
}

TEST(SymExprCompare, FullShiftCountAndBooleanCarrierWidthsRemainObservable) {
  SymContext C;
  SymRef X = C.mkVar("x", 32), Y = C.mkVar("y", 32);
  SymRef D = C.mkSub(X, Y);
  SymRef V = C.mkAnd(C.mkXor(X, Y), C.mkXor(X, D));
  SymRef S = C.mkXor(D, V);
  llvm::APInt LargeCount(256, 31);
  LargeCount.setBit(128);
  EXPECT_EQ(C.mkLShr(S, C.mkConst(LargeCount)), C.mkZero(32));
  EXPECT_EQ(C.mkAShr(S, C.mkConst(LargeCount)), C.mkSExt(C.mkSlt(X, Y), 32));
  SymRef OneBit = C.mkVar("bit", 1);
  EXPECT_EQ(C.mkAShr(OneBit, C.mkVar("count", 256)), OneBit);
  SymRef WideFlag = C.mkNe(C.mkZExt(C.mkVar("three_bits", 3), 8), C.mkZero(8));
  SymRef Sign = C.mkSlt(D, C.mkZero(32));
  SymRef Unrelated = C.mkXor(Sign, WideFlag);
  EXPECT_NE(Unrelated, C.mkSlt(X, Y));
  const size_t Before = C.numNodes();
  EXPECT_FALSE(detail::recoverObservedComparison(
      C, SymOp::Xor, {Sign, WideFlag}, llvm::APInt(1, 0)));
  EXPECT_EQ(C.numNodes(), Before);
}

} // namespace
