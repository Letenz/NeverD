//===- SymExprMaskTests.cpp - Word-mask canonicalization ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymExpr.h"
#include "neverd/symbolic/SymMBA.h"

using namespace neverd::symbolic;

namespace {

constexpr uint32_t Widths[] = {1, 3, 8, 32, 64, 128, 256};

TEST(SymExprMask, CombinesDisjointMasksOnTheSameSource) {
  for (uint32_t W : Widths) {
    SCOPED_TRACE(W);
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", W);
    SymRef Y = Ctx.mkVar("y", W);
    llvm::APInt M(W, 0);
    for (uint32_t Bit = 0; Bit < W; Bit += 2)
      M.setBit(Bit);
    SymRef A = Ctx.mkAnd(X, Ctx.mkConst(M));
    SymRef B = Ctx.mkAnd(X, Ctx.mkConst(~M));
    EXPECT_EQ(Ctx.mkAdd(A, B), X);
    EXPECT_EQ(Ctx.mkOr(A, B), X);
    EXPECT_EQ(Ctx.mkAdd({Y, A, B, X}),
              Ctx.mkAdd(Y, Ctx.mkMul(Ctx.mkConst(W, 2), X)));

    // A source that is itself an AND has a flattened operand list.
    SymRef Source = Ctx.mkAnd(X, Y);
    A = Ctx.mkAnd(Source, Ctx.mkConst(M));
    B = Ctx.mkAnd(Source, Ctx.mkConst(~M));
    EXPECT_EQ(Ctx.mkAdd(A, B), Source);
    EXPECT_EQ(Ctx.mkOr(A, B), Source);
  }
}

TEST(SymExprMask, OrAcceptsOverlapButAdditionKeepsCarriesAndOtherSources) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 8);
  SymRef Y = Ctx.mkVar("y", 8);
  SymRef A = Ctx.mkAnd(X, Ctx.mkConst(8, 3));
  SymRef B = Ctx.mkAnd(X, Ctx.mkConst(8, 5));
  EXPECT_EQ(Ctx.mkOr(A, B), Ctx.mkAnd(X, Ctx.mkConst(8, 7)));
  EXPECT_NE(Ctx.mkAdd(A, B), Ctx.mkOr(A, B));
  EXPECT_EQ(Ctx.evalU64(Ctx.mkAdd(A, B), {1, 0}), 2u);
  EXPECT_EQ(Ctx.evalU64(Ctx.mkOr(A, B), {1, 0}), 1u);
  EXPECT_EQ(Ctx.mkAdd(Ctx.mkAnd(X, Ctx.mkConst(8, 15)),
                      Ctx.mkAnd(X, Ctx.mkConst(8, 48))),
            Ctx.mkAnd(X, Ctx.mkConst(8, 63)));

  SymRef Different = Ctx.mkAdd(Ctx.mkAnd(X, Ctx.mkConst(8, 15)),
                               Ctx.mkAnd(Y, Ctx.mkConst(8, 240)));
  EXPECT_NE(Different, X);
  EXPECT_EQ(Ctx.evalU64(Different, {0, 255}), 240u);
  // Repeated summands have multiplicity even when another mask is disjoint.
  EXPECT_EQ(Ctx.evalU64(Ctx.mkAdd({A, A, B}), {1, 0}), 3u);
}

TEST(SymExprMask, LowMaskDemandOnlyChangesTheMaskedConsumer) {
  for (uint32_t W : Widths) {
    SCOPED_TRACE(W);
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", W);
    SymRef Y = Ctx.mkVar("y", W);
    uint32_t K = W == 1 ? 1 : W / 2;
    llvm::APInt L = llvm::APInt::getLowBitsSet(W, K);
    SymRef Mask = Ctx.mkConst(L);
    SymRef Shared = Ctx.mkAdd(Ctx.mkAnd(X, Mask), Ctx.mkAnd(Y, Mask));
    auto OriginalOperands = Ctx.operands(Shared);
    llvm::SmallVector<SymRef, 4> Saved(OriginalOperands.begin(),
                                       OriginalOperands.end());
    SymRef Masked = Ctx.mkAnd(Shared, Mask);
    EXPECT_EQ(Masked, Ctx.mkAnd(Ctx.mkAdd(X, Y), Mask));
    EXPECT_EQ(Ctx.operands(Shared), llvm::ArrayRef<SymRef>(Saved));
    if (W > 1) {
      llvm::APInt High = llvm::APInt::getOneBitSet(W, K);
      llvm::APInt Values[] = {High, llvm::APInt(W, 0)};
      EXPECT_EQ(Ctx.eval(Shared, Values), llvm::APInt(W, 0));
      EXPECT_EQ(Ctx.eval(Ctx.mkAdd(X, Y), Values), High);

      // An input mask can contain more bits than the demanded low prefix.
      SymRef Wider = Ctx.mkConst(L | llvm::APInt::getSignMask(W));
      EXPECT_EQ(
          Ctx.mkAnd(Ctx.mkAdd(Ctx.mkAnd(X, Wider), Ctx.mkAnd(Y, Mask)), Mask),
          Masked);
    }
  }
}

TEST(SymExprMask, HoleyMasksAndIncompleteInputMasksKeepTheirCarrySources) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 8);
  SymRef Y = Ctx.mkVar("y", 8);
  SymRef Holey = Ctx.mkConst(8, 5);
  SymRef Original =
      Ctx.mkAnd(Ctx.mkAdd(Ctx.mkAnd(X, Holey), Ctx.mkAnd(Y, Holey)), Holey);
  SymRef Incorrect = Ctx.mkAnd(Ctx.mkAdd(X, Y), Holey);
  EXPECT_NE(Original, Incorrect);
  EXPECT_EQ(Ctx.evalU64(Original, {2, 2}), 0u);
  EXPECT_EQ(Ctx.evalU64(Incorrect, {2, 2}), 4u);

  SymRef Low = Ctx.mkConst(8, 3);
  SymRef Partial = Ctx.mkConst(8, 2);
  Original =
      Ctx.mkAnd(Ctx.mkAdd(Ctx.mkAnd(X, Partial), Ctx.mkAnd(Y, Partial)), Low);
  EXPECT_EQ(Ctx.evalU64(Original, {1, 1}), 0u);
  EXPECT_EQ(Ctx.evalU64(Ctx.mkAnd(Ctx.mkAdd(X, Y), Low), {1, 1}), 2u);
}

TEST(SymExprMask, ReconstructsLogicalSlicesUsingFullWidthCounts) {
  for (uint32_t W : Widths) {
    SCOPED_TRACE(W);
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", W);
    for (uint32_t K : {0u, W / 2, W - 1, W, W + 1}) {
      SCOPED_TRACE(K);
      for (uint32_t CountWidth : {1u, 3u, 8u, 32u, 128u, 256u}) {
        if (llvm::APInt(32, K).getActiveBits() > CountWidth)
          continue;
        SCOPED_TRACE(CountWidth);
        SymRef Count = Ctx.mkConst(CountWidth, K);
        SymRef ShiftBack = Ctx.mkShl(Ctx.mkLShr(X, Count), Count);
        SymRef ShiftOut = Ctx.mkLShr(Ctx.mkShl(X, Count), Count);
        llvm::APInt High =
            K >= W ? llvm::APInt(W, 0) : ~llvm::APInt::getLowBitsSet(W, K);
        llvm::APInt Low =
            K >= W ? llvm::APInt(W, 0) : llvm::APInt::getLowBitsSet(W, W - K);
        EXPECT_EQ(ShiftBack, Ctx.mkAnd(X, Ctx.mkConst(High)));
        EXPECT_EQ(ShiftOut, Ctx.mkAnd(X, Ctx.mkConst(Low)));
        if (K < W) {
          EXPECT_EQ(Ctx.mkOr(ShiftBack, Ctx.mkAnd(X, Ctx.mkConst(~High))), X);
          EXPECT_EQ(Ctx.mkAdd(ShiftOut, Ctx.mkAnd(X, Ctx.mkConst(~Low))), X);
        }
      }
    }
    SymRef Huge = Ctx.mkConst(llvm::APInt::getOneBitSet(256, 200));
    EXPECT_EQ(Ctx.mkLShr(Ctx.mkShl(X, Ctx.mkConst(32, 1)), Huge),
              Ctx.mkZero(W));
  }
}

TEST(SymExprMask,
     ShiftReconstructionRejectsOtherShapesAndArithmeticRightShift) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 8);
  SymRef Count = Ctx.mkConst(32, 4);
  SymRef Signed = Ctx.mkAShr(Ctx.mkShl(X, Count), Count);
  SymRef Logical = Ctx.mkLShr(Ctx.mkShl(X, Count), Count);
  EXPECT_EQ(Ctx.evalU64(Signed, {8}), 248u);
  EXPECT_EQ(Ctx.evalU64(Logical, {8}), 8u);
  EXPECT_NE(Signed, Logical);
  SymRef Mismatch = Ctx.mkMul(Ctx.mkConst(8, 8), Ctx.mkLShr(X, Count));
  EXPECT_EQ(Ctx.evalU64(Mismatch, {255}), 120u);
  EXPECT_NE(Mismatch, Ctx.mkAnd(X, Ctx.mkConst(8, 240)));
  SymRef NonPower = Ctx.mkLShr(Ctx.mkMul(Ctx.mkConst(8, 17), X), Count);
  EXPECT_EQ(Ctx.evalU64(NonPower, {16}), 1u);
  SymRef Y = Ctx.mkVar("y", 8);
  SymRef ExtraFactor = Ctx.mkLShr(Ctx.mkMul({Ctx.mkConst(8, 16), X, Y}), Count);
  EXPECT_EQ(Ctx.evalU64(ExtraFactor, {3, 2}), 6u);
}

TEST(SymExprMask, ExhaustiveSmallWordsMatchOriginalOperations) {
  for (uint32_t W : {1u, 2u, 3u, 4u}) {
    uint32_t Limit = 1u << W;
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", W);
    SymRef Y = Ctx.mkVar("y", W);
    for (uint32_t M = 0; M < Limit; ++M)
      for (uint32_t N = 0; N < Limit; ++N) {
        SymRef A = Ctx.mkAnd(X, Ctx.mkConst(W, M));
        SymRef B = Ctx.mkAnd(X, Ctx.mkConst(W, N));
        SymRef Sum = Ctx.mkAdd(A, B);
        SymRef Union = Ctx.mkOr(A, B);
        SymRef Masked = Ctx.mkAnd(Ctx.mkAdd(A, Ctx.mkAnd(Y, Ctx.mkConst(W, M))),
                                  Ctx.mkConst(W, N));
        for (uint32_t V = 0; V < Limit; ++V) {
          EXPECT_EQ(Ctx.evalU64(Sum, {V, 0}), ((V & M) + (V & N)) % Limit);
          EXPECT_EQ(Ctx.evalU64(Union, {V, 0}), (V & M) | (V & N));
          for (uint32_t U = 0; U < Limit; ++U)
            EXPECT_EQ(Ctx.evalU64(Masked, {V, U}), ((V & M) + (U & M)) & N);
        }
      }
  }
}

TEST(SymExprMask, SharedDeepSourcesAreOpaqueAndMBARequiresNoSamples) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 32);
  SymRef Y = Ctx.mkVar("y", 32);
  SymRef Source = X;
  for (unsigned I = 0; I < 10000; ++I)
    Source = Ctx.mkUDiv(Source, Y);
  SymRef Low = Ctx.mkConst(32, 65535);
  SymRef High = Ctx.mkConst(32, 0xffff0000);
  SymRef A = Ctx.mkAnd(Source, Low);
  SymRef B = Ctx.mkAnd(Source, High);
  size_t Before = Ctx.numNodes();
  EXPECT_EQ(Ctx.mkAdd(A, B), Source);
  EXPECT_EQ(Ctx.mkOr(A, B), Source);
  EXPECT_EQ(Ctx.numNodes(), Before);
  MBAOptions Options;
  Options.VerifySamples = 0;
  SymRef Masked =
      Ctx.mkAnd(Ctx.mkAdd(Ctx.mkAnd(X, Low), Ctx.mkAnd(Y, Low)), Low);
  EXPECT_EQ(simplifyMBA(Ctx, Masked, Options).Expr,
            Ctx.mkAnd(Ctx.mkAdd(X, Y), Low));
}

TEST(SymExprMask, CollectedShiftSlicesKeepSumsCanonical) {
  for (uint32_t W : {3u, 8u, 32u, 64u, 128u, 256u}) {
    SCOPED_TRACE(W);
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", W);
    SymRef Y = Ctx.mkVar("y", W);
    SymRef Count = Ctx.mkConst(32, 1);
    SymRef Shifted = Ctx.mkLShr(X, Count);
    SymRef Slice = Ctx.mkAnd(X, Ctx.mkConst(~llvm::APInt(W, 1)));
    SymRef Sum = Ctx.mkAdd({Shifted, Shifted, Slice});
    EXPECT_EQ(Sum, Ctx.mkMul(Ctx.mkConst(W, 2), Slice));
    EXPECT_EQ(Ctx.rebuild(Sum, Ctx.operands(Sum)), Sum);
    EXPECT_EQ(Ctx.mkAdd({Shifted, Shifted, Ctx.mkNeg(Slice)}), Ctx.mkZero(W));
    if (W <= 8)
      for (uint64_t Value = 0; Value < (uint64_t(1) << W); ++Value)
        EXPECT_EQ(Ctx.evalU64(Sum, {Value, 0}),
                  ((Value >> 1) * 2 + (Value & ~uint64_t(1))) &
                      ((uint64_t(1) << W) - 1));

    SymRef ZeroShift = Ctx.mkLShr(Ctx.mkAnd(X, Ctx.mkConst(W, 1)), Count);
    EXPECT_EQ(Ctx.mkAdd({ZeroShift, ZeroShift, Y}), Y);
    EXPECT_EQ(Ctx.mkAdd(ZeroShift, ZeroShift), Ctx.mkZero(W));
  }
}

TEST(SymExprMask, LowMaskDemandPreservesRepeatedSharedSums) {
  SymContext Ctx;
  llvm::SmallVector<SymRef, 8> Variables;
  for (unsigned I = 0; I < 1024; ++I)
    Variables.push_back(Ctx.mkVar("x" + std::to_string(I), 32));
  SymRef Shared = Ctx.mkAdd(Variables);
  SymRef Low = Ctx.mkConst(32, 255);
  llvm::SmallVector<SymRef, 8> MaskedTerms;
  for (unsigned I = 0; I < 257; ++I)
    MaskedTerms.push_back(Ctx.mkAnd(Shared, Ctx.mkConst(32, 255 | (I << 8))));
  SymRef Sum = Ctx.mkAdd(MaskedTerms);
  size_t Before = Ctx.numNodes();
  SymRef Masked = Ctx.mkAnd(Sum, Low);
  EXPECT_LE(Ctx.numNodes() - Before, 4u);
  EXPECT_EQ(Masked, Ctx.mkAnd(Ctx.mkMul(Ctx.mkConst(32, 257), Shared), Low));
  EXPECT_EQ(Ctx.operands(Shared), llvm::ArrayRef<SymRef>(Variables));
  for (uint64_t Value : {0u, 1u, 255u, 256u, 0xffffffffu}) {
    llvm::SmallVector<llvm::APInt, 8> Values(Variables.size(),
                                             llvm::APInt(32, Value));
    // Keep a nonzero low byte even though the shared sum has 1024 inputs.
    Values.front() = llvm::APInt(32, uint32_t(Value + 1));
    EXPECT_EQ(Ctx.eval(Masked, Values),
              Ctx.eval(Sum, Values) & llvm::APInt(32, 255));
  }

  SymContext Narrow;
  SymRef X = Narrow.mkVar("x", 8);
  SymRef Y = Narrow.mkVar("y", 8);
  SymRef Source = Narrow.mkAdd(X, Y);
  SymRef M1 = Narrow.mkConst(8, 1);
  SymRef A = Narrow.mkAnd(Source, M1);
  SymRef B = Narrow.mkAnd(Source, Narrow.mkConst(8, 3));
  SymRef FullConsumer = Narrow.mkXor(Source, Narrow.mkConst(8, 128));
  for (uint32_t Count : {256u, 257u}) {
    llvm::SmallVector<SymRef, 8> Repeated(128, A);
    Repeated.append(Count - 128, B);
    SymRef Original = Narrow.mkAdd(Repeated);
    SymRef Reduced = Narrow.mkAnd(Original, M1);
    EXPECT_EQ(Reduced, Count == 256 ? Narrow.mkZero(8) : A);
    for (uint64_t V = 0; V < 256; ++V) {
      EXPECT_EQ(Narrow.evalU64(Reduced, {V, 1}),
                Narrow.evalU64(Original, {V, 1}) & 1);
      EXPECT_EQ(Narrow.evalU64(FullConsumer, {V, 1}), ((V + 1) & 255) ^ 128);
    }
  }
  SymRef Negative = Narrow.mkAdd(Narrow.mkMul(Narrow.mkConst(8, 253), A),
                                 Narrow.mkMul(Narrow.mkConst(8, 2), B));
  EXPECT_EQ(Narrow.mkAnd(Negative, M1), Narrow.mkAnd(Narrow.mkNeg(Source), M1));
}

TEST(SymExprMask, ComposedMaskRulesDoNotReenterDeepSources) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 8);
  SymRef Source = X;
  SymRef M5 = Ctx.mkConst(8, 5);
  SymRef M2 = Ctx.mkConst(8, 2);
  SymRef M23 = Ctx.mkConst(8, 23);
  SymRef Low = Ctx.mkConst(8, 7);
  for (unsigned I = 0; I < 10000; ++I)
    Source = Ctx.mkAdd(
        {Ctx.mkAnd(Source, M5), Ctx.mkAnd(Source, M2), Ctx.mkAnd(Source, M23)});
  size_t Before = Ctx.numNodes();
  SymRef Masked = Ctx.mkAnd(Source, Low);
  // Removing M23 exposes a merge of M5 and M2. That merge must not call back
  // into demanded-add rewriting through every preceding shared source.
  EXPECT_LE(Ctx.numNodes() - Before, 8u);
  for (uint32_t V : {0u, 1u, 7u, 8u, 31u, 255u}) {
    uint32_t Expected = V;
    for (unsigned I = 0; I < 10000; ++I)
      Expected = ((Expected & 5) + (Expected & 2) + (Expected & 23)) & 255;
    EXPECT_EQ(Ctx.evalU64(Source, {V}), Expected);
    EXPECT_EQ(Ctx.evalU64(Masked, {V}), Expected & 7);
  }
}

} // namespace
