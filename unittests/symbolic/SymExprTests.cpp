//===- SymExprTests.cpp - Interning and canonicalisation ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Covers the two properties the rest of the optimiser is allowed to assume:
/// that structurally equal expressions are the same \c SymRef, and that every
/// way of writing the same value reaches the same node without any rewrite
/// pass running.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymExpr.h"

#include <random>

using namespace neverd::symbolic;

namespace {

constexpr uint32_t W32 = 32;

TEST(SymExpr, InterningMakesStructuralEqualityAPointerCompare) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);

  EXPECT_EQ(X, Ctx.mkVar("x", W32));
  EXPECT_NE(X, Y);
  EXPECT_EQ(Ctx.mkXor(X, Y), Ctx.mkXor(X, Y));
  EXPECT_EQ(Ctx.mkConst(W32, 7), Ctx.mkConst(W32, 7));
  // Same value, different width, different node.
  EXPECT_NE(Ctx.mkConst(W32, 7), Ctx.mkConst(8, 7));
}

TEST(SymExpr, CommutativeOperandsAreSorted) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);
  SymRef Z = Ctx.mkVar("z", W32);

  EXPECT_EQ(Ctx.mkAdd(X, Y), Ctx.mkAdd(Y, X));
  EXPECT_EQ(Ctx.mkAnd(X, Y), Ctx.mkAnd(Y, X));
  EXPECT_EQ(Ctx.mkOr(X, Y), Ctx.mkOr(Y, X));
  EXPECT_EQ(Ctx.mkXor(X, Y), Ctx.mkXor(Y, X));
  EXPECT_EQ(Ctx.mkMul(X, Y), Ctx.mkMul(Y, X));

  // Associativity is flattened, so the grouping an obfuscator chose is gone.
  EXPECT_EQ(Ctx.mkAdd(Ctx.mkAdd(X, Y), Z), Ctx.mkAdd(X, Ctx.mkAdd(Y, Z)));
  EXPECT_EQ(Ctx.mkXor(Ctx.mkXor(Z, X), Y), Ctx.mkXor(X, Ctx.mkXor(Y, Z)));
}

TEST(SymExpr, ConstantsFoldOnConstruction) {
  SymContext Ctx;
  EXPECT_EQ(Ctx.mkAdd(Ctx.mkConst(W32, 3), Ctx.mkConst(W32, 4)),
            Ctx.mkConst(W32, 7));
  EXPECT_EQ(Ctx.mkMul(Ctx.mkConst(W32, 3), Ctx.mkConst(W32, 4)),
            Ctx.mkConst(W32, 12));
  EXPECT_EQ(Ctx.mkNot(Ctx.mkConst(W32, 0)), Ctx.mkOnes(W32));
  // Wrapping is modulo the width, not an overflow.
  EXPECT_EQ(Ctx.mkAdd(Ctx.mkOnes(W32), Ctx.mkConst(W32, 1)), Ctx.mkZero(W32));
}

TEST(SymExpr, SumsCollectLikeTerms) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);

  EXPECT_EQ(Ctx.mkAdd(X, X), Ctx.mkMul(Ctx.mkConst(W32, 2), X));
  EXPECT_EQ(Ctx.mkAdd(X, Ctx.mkMul(Ctx.mkConst(W32, 2), X)),
            Ctx.mkMul(Ctx.mkConst(W32, 3), X));
  EXPECT_EQ(Ctx.mkSub(X, X), Ctx.mkZero(W32));
  EXPECT_EQ(Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(W32, 3), X),
                      Ctx.mkMul(Ctx.mkOnes(W32), X)),
            Ctx.mkMul(Ctx.mkConst(W32, 2), X));
  // A shared base with a compound body still collects.
  SymRef XY = Ctx.mkXor(X, Y);
  EXPECT_EQ(Ctx.mkAdd(XY, XY), Ctx.mkMul(Ctx.mkConst(W32, 2), XY));
}

TEST(SymExpr, ShiftsByConstantsBecomeProductsSoSumsCanCollectThem) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);

  EXPECT_EQ(Ctx.mkShl(X, Ctx.mkConst(W32, 1)),
            Ctx.mkMul(Ctx.mkConst(W32, 2), X));
  // This is the point of the normalisation: `x + (x << 1)` is `3*x`.
  EXPECT_EQ(Ctx.mkAdd(X, Ctx.mkShl(X, Ctx.mkConst(W32, 1))),
            Ctx.mkMul(Ctx.mkConst(W32, 3), X));
  // A shift past the width is zero, matching QF_BV rather than any one CPU.
  EXPECT_EQ(Ctx.mkShl(X, Ctx.mkConst(W32, 32)), Ctx.mkZero(W32));
  EXPECT_EQ(Ctx.mkLShr(X, Ctx.mkConst(W32, 99)), Ctx.mkZero(W32));
}

TEST(SymExpr, RightShiftDiscardsOnlyNeutralBitwiseWindows) {
  for (unsigned Width : {8U, 16U, 32U, 64U, 128U}) {
    SCOPED_TRACE(Width);
    SymContext C;
    const unsigned Half = Width / 2;
    auto X = C.mkVar("x", Half), Y = C.mkVar("y", Half);
    auto High = C.mkShl(C.mkZExt(X, Width), C.mkConst(Width, Half));
    auto Low = C.mkZExt(Y, Width);
    auto Count = C.mkConst(128, Half);
    for (auto Packed : {C.mkOr(High, Low), C.mkXor(High, Low)})
      EXPECT_EQ(C.mkExtract(C.mkLShr(Packed, Count), 0, Half), X);
    auto Neutral =
        C.mkOr(Low, C.mkConst(llvm::APInt::getHighBitsSet(Width, Half)));
    EXPECT_EQ(C.mkExtract(C.mkLShr(C.mkAnd(High, Neutral), Count), 0, Half), X);
    // Several discarded terms may jointly cover more than one machine word.
    auto Narrow = C.mkVar("narrow", 1);
    auto Whole = C.mkVar("whole", Width);
    EXPECT_EQ(
        C.mkLShr(C.mkOr(Whole, C.mkZExt(Narrow, Width)), C.mkConst(Width, 1)),
        C.mkLShr(Whole, C.mkConst(Width, 1)));
  }
}

TEST(SymExpr, RightShiftWindowFoldsMatchIndependentByteArithmetic) {
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  for (unsigned Shift = 1; Shift < 8; ++Shift) {
    const unsigned Mask = (1U << Shift) - 1;
    auto Low = C.mkAnd(Y, C.mkConst(8, Mask));
    auto HighOnes = C.mkOr(Y, C.mkConst(8, 255U ^ Mask));
    auto Count = C.mkConst(8, Shift);
    const SymRef Results[] = {C.mkLShr(C.mkOr(X, Low), Count),
                              C.mkLShr(C.mkXor(X, Low), Count),
                              C.mkLShr(C.mkAnd(X, HighOnes), Count)};
    for (auto R : Results)
      EXPECT_EQ(R, C.mkLShr(X, Count));
    // Addition can carry out of the discarded bits; a partly retained OR
    // operand, a sign extension and a variable shift cannot be dropped.
    const SymRef Unchanged[] = {
        C.mkLShr(C.mkAdd(X, Low), Count),
        C.mkLShr(C.mkOr(X, C.mkAnd(Y, C.mkConst(8, Mask | (1U << Shift)))),
                 Count),
        C.mkLShr(C.mkOr(X, C.mkSExt(C.mkExtract(Y, 0, 4), 8)), Count),
        C.mkLShr(C.mkOr(X, Low), Y)};
    for (unsigned A = 0; A < 256; ++A)
      for (unsigned B = 0; B < 256; ++B) {
        uint64_t Values[] = {A, B};
        for (auto R : Results)
          ASSERT_EQ(C.evalU64(R, Values), A >> Shift);
        const unsigned SignedNibble = (B & 8) ? (B & 15) | 240 : B & 15;
        const unsigned Expected[] = {
            ((A + (B & Mask)) & 255) >> Shift,
            (A | (B & (Mask | (1U << Shift)))) >> Shift,
            (A | SignedNibble) >> Shift, B >= 8 ? 0 : (A | (B & Mask)) >> B};
        for (unsigned I = 0; I < std::size(Unchanged); ++I)
          ASSERT_EQ(C.evalU64(Unchanged[I], Values), Expected[I]);
      }
  }
}

TEST(SymExpr, RightShiftWindowDiscoveryIsBoundedAndKeepsFullCounts) {
  SymContext C;
  auto X = C.mkVar("x", 64);
  llvm::SmallVector<SymRef, 10> Terms{X};
  for (unsigned I = 0; I < 8; ++I)
    Terms.push_back(C.mkZExt(C.mkFreshVar(8), 64));
  auto Many = C.mkOr(Terms);
  auto Count = C.mkConst(64, 32);
  auto Result = C.mkLShr(Many, Count);
  EXPECT_EQ(C.op(Result), SymOp::LShr);
  EXPECT_EQ(C.operand(Result, 0), Many);
  Terms.pop_back();
  EXPECT_EQ(C.mkLShr(C.mkOr(Terms), Count), C.mkLShr(X, Count));
  auto Wide = C.mkOr(C.mkVar("wide", 256), C.mkZExt(C.mkVar("low", 8), 256));
  EXPECT_EQ(C.operand(C.mkLShr(Wide, C.mkConst(256, 128)), 0), Wide);
  EXPECT_EQ(C.mkLShr(Many, C.mkConst(llvm::APInt::getOneBitSet(128, 80))),
            C.mkZero(64));
  EXPECT_EQ(C.mkLShr(Many, C.mkZero(128)), Many);
}

TEST(SymExpr, XorClearsBitsForcedByTheSameOrMask) {
  for (unsigned Width : {1u, 3u, 8u, 32u, 64u, 128u, 256u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width);
    SymRef Y = Ctx.mkVar("y", Width);
    llvm::APInt Mask(Width, 0);
    for (unsigned I = 0; I < Width; I += 2)
      Mask.setBit(I);
    SymRef M = Ctx.mkConst(Mask);
    SymRef Keep = Ctx.mkConst(~Mask);
    EXPECT_EQ(Ctx.mkXor(Ctx.mkOr(X, M), M), Ctx.mkAnd(X, Keep));
    EXPECT_EQ(Ctx.mkXor(M, Ctx.mkOr({M, X, Y})),
              Ctx.mkAnd(Ctx.mkOr(X, Y), Keep));
    EXPECT_EQ(Ctx.mkXor(Ctx.mkOr(X, Ctx.mkZero(Width)), Ctx.mkZero(Width)), X);
    EXPECT_EQ(Ctx.mkXor(Ctx.mkOr(X, Ctx.mkOnes(Width)), Ctx.mkOnes(Width)),
              Ctx.mkZero(Width));
    llvm::APInt Values[] = {llvm::APInt::getAllOnes(Width), Mask};
    SymRef DifferentMask = Ctx.mkXor(Ctx.mkOr(X, M), Keep);
    EXPECT_EQ(Ctx.eval(DifferentMask, Values), (Values[0] | Mask) ^ ~Mask);
    EXPECT_NE(Ctx.eval(DifferentMask, Values), Values[0] & ~Mask);
  }
}

TEST(SymExpr, ForcedOrBitClearingMatchesSmallWordArithmetic) {
  for (unsigned Width : {1u, 2u, 3u, 4u}) {
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width);
    unsigned Limit = 1u << Width;
    for (unsigned Mask = 0; Mask < Limit; ++Mask) {
      SymRef M = Ctx.mkConst(Width, Mask);
      SymRef Result = Ctx.mkXor(Ctx.mkOr(X, M), M);
      for (unsigned Value = 0; Value < Limit; ++Value) {
        uint64_t Values[] = {Value};
        EXPECT_EQ(Ctx.evalU64(Result, Values), (Value | Mask) ^ Mask);
      }
    }
  }
}

TEST(SymExpr, RotatesAcceptIndependentlySizedAmounts) {
  struct Case {
    uint32_t Width;
    llvm::APInt Amount;
    uint64_t Left;
    uint64_t Right;
  };
  const Case Cases[] = {
      {3, llvm::APInt(1, 1), 2, 4},
      {3, llvm::APInt(2, 3), 1, 1},
      {3, llvm::APInt(8, 4), 2, 4},
      {3, llvm::APInt::getOneBitSet(128, 64), 2, 4},
      {3, llvm::APInt::getOneBitSet(128, 127), 4, 2},
      {3, llvm::APInt::getAllOnes(128), 1, 1},
      {1, llvm::APInt::getAllOnes(128), 1, 1},
  };
  for (const Case &T : Cases) {
    SCOPED_TRACE(T.Width);
    SCOPED_TRACE(T.Amount.getBitWidth());
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", T.Width);
    SymRef Y = Ctx.mkVar("amount", T.Amount.getBitWidth());
    SymRef One = Ctx.mkConst(T.Width, 1);
    SymRef Amount = Ctx.mkConst(T.Amount);
    EXPECT_EQ(Ctx.mkRol(One, Amount), Ctx.mkConst(T.Width, T.Left));
    EXPECT_EQ(Ctx.mkRor(One, Amount), Ctx.mkConst(T.Width, T.Right));

    llvm::APInt Values[] = {llvm::APInt(T.Width, 1), T.Amount};
    EXPECT_EQ(Ctx.eval(Ctx.mkRol(X, Y), Values), llvm::APInt(T.Width, T.Left));
    EXPECT_EQ(Ctx.eval(Ctx.mkRor(X, Y), Values), llvm::APInt(T.Width, T.Right));
    if (T.Amount.getBitWidth() <= 64) {
      uint64_t ValuesU64[] = {1, T.Amount.getZExtValue()};
      EXPECT_EQ(Ctx.evalU64(Ctx.mkRol(X, Y), ValuesU64), T.Left);
      EXPECT_EQ(Ctx.evalU64(Ctx.mkRor(X, Y), ValuesU64), T.Right);
    }
  }
}

TEST(SymExpr, ShiftsKeepHighBitsOfIndependentlySizedAmounts) {
  const llvm::APInt Amounts[] = {llvm::APInt(16, 256),
                                 llvm::APInt::getOneBitSet(128, 64)};
  for (const llvm::APInt &Amount : Amounts) {
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", 8);
    SymRef Y = Ctx.mkVar("amount", Amount.getBitWidth());
    SymRef Value = Ctx.mkConst(8, 0x81);
    SymRef Count = Ctx.mkConst(Amount);
    EXPECT_EQ(Ctx.mkShl(Value, Count), Ctx.mkZero(8));
    EXPECT_EQ(Ctx.mkLShr(Value, Count), Ctx.mkZero(8));
    EXPECT_EQ(Ctx.mkAShr(Value, Count), Ctx.mkOnes(8));

    llvm::APInt Values[] = {llvm::APInt(8, 0x81), Amount};
    EXPECT_EQ(Ctx.eval(Ctx.mkShl(X, Y), Values), llvm::APInt(8, 0));
    EXPECT_EQ(Ctx.eval(Ctx.mkLShr(X, Y), Values), llvm::APInt(8, 0));
    EXPECT_EQ(Ctx.eval(Ctx.mkAShr(X, Y), Values), llvm::APInt(8, 0xff));
    if (Amount.getBitWidth() <= 64) {
      uint64_t ValuesU64[] = {0x81, Amount.getZExtValue()};
      EXPECT_EQ(Ctx.evalU64(Ctx.mkShl(X, Y), ValuesU64), 0u);
      EXPECT_EQ(Ctx.evalU64(Ctx.mkLShr(X, Y), ValuesU64), 0u);
      EXPECT_EQ(Ctx.evalU64(Ctx.mkAShr(X, Y), ValuesU64), 0xffu);
    }
  }
}

TEST(SymExpr, BitwiseIdentitiesHoldOnConstruction) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);

  EXPECT_EQ(Ctx.mkXor(X, X), Ctx.mkZero(W32));
  EXPECT_EQ(Ctx.mkAnd(X, X), X);
  EXPECT_EQ(Ctx.mkOr(X, X), X);
  EXPECT_EQ(Ctx.mkAnd(X, Ctx.mkZero(W32)), Ctx.mkZero(W32));
  EXPECT_EQ(Ctx.mkAnd(X, Ctx.mkOnes(W32)), X);
  EXPECT_EQ(Ctx.mkOr(X, Ctx.mkOnes(W32)), Ctx.mkOnes(W32));
  EXPECT_EQ(Ctx.mkOr(X, Ctx.mkZero(W32)), X);
  EXPECT_EQ(Ctx.mkNot(Ctx.mkNot(X)), X);
  EXPECT_EQ(Ctx.mkXor(X, Ctx.mkZero(W32)), X);
}

TEST(SymExpr, MaskedOrKeepsDisjointKnownBits) {
  SymContext Ctx;
  SymRef Quotient = Ctx.mkVar("quotient", 16);
  SymRef Top = Ctx.mkVar("top", 16);
  SymRef Status =
      Ctx.mkOr(Ctx.mkConst(16, 0x0400),
               Ctx.mkOr(Ctx.mkAnd(Quotient, Ctx.mkConst(16, 0x4300)),
                        Ctx.mkAnd(Top, Ctx.mkConst(16, 0x3800))));

  EXPECT_EQ(Ctx.mkAnd(Status, Ctx.mkConst(16, 0x0400)),
            Ctx.mkConst(16, 0x0400));
  EXPECT_FALSE(Ctx.isConst(Ctx.mkAnd(Status, Ctx.mkConst(16, 0x0100))));
  EXPECT_FALSE(Ctx.isConst(Ctx.mkAnd(Status, Ctx.mkConst(16, 0x0800))));
}

TEST(SymExpr, StructuralOperatorsCollapseWhereTheyCan) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);

  EXPECT_EQ(Ctx.mkExtract(X, 0, W32), X);
  EXPECT_EQ(Ctx.mkExtract(Ctx.mkExtract(X, 8, 16), 4, 8),
            Ctx.mkExtract(X, 12, 8));
  EXPECT_EQ(Ctx.mkZExt(Ctx.mkZExt(Ctx.mkExtract(X, 0, 8), 16), W32),
            Ctx.mkZExt(Ctx.mkExtract(X, 0, 8), W32));
  // Adjacent literals in a concatenation merge into one word.
  EXPECT_EQ(Ctx.mkConcat(Ctx.mkConst(8, 0xAB), Ctx.mkConst(8, 0xCD)),
            Ctx.mkConst(16, 0xABCD));
  EXPECT_EQ(
      Ctx.width(Ctx.mkConcat(Ctx.mkExtract(X, 0, 8), Ctx.mkExtract(X, 8, 8))),
      16u);
}

TEST(SymExpr, ZeroPrefixedWordsShareTheZeroExtensionForm) {
  for (uint32_t Bits : {1u, 7u, 8u, 13u, 24u, 32u, 63u, 64u, 127u, 255u})
    for (uint32_t Padding : {1u, 7u, 8u, 19u, 64u}) {
      SCOPED_TRACE(Bits);
      SCOPED_TRACE(Padding);
      SymContext Ctx;
      const auto X = Ctx.mkVar("x", Bits);
      const auto Value = Ctx.mkAdd(X, Ctx.mkConst(Bits, 3));
      const auto Expected = Ctx.mkZExt(Value, Bits + Padding);
      EXPECT_EQ(Ctx.mkConcat(Ctx.mkZero(Padding), Value), Expected);
      EXPECT_EQ(Ctx.mkConcat({Ctx.mkZero(Padding), Ctx.mkZero(5), Value}),
                Ctx.mkZExt(Value, Bits + Padding + 5));
      llvm::APInt Inputs[] = {llvm::APInt::getAllOnes(Bits)};
      EXPECT_EQ(Ctx.eval(Expected, Inputs),
                (Inputs[0] + llvm::APInt(Bits, 3)).zext(Bits + Padding));
    }
}

TEST(SymExpr, ExtendedConcatGroupingPreservesEveryNonzeroAndUnknownBit) {
  SymContext Ctx;
  const auto High = Ctx.mkVar("high", 8), Low = Ctx.mkVar("low", 8);
  const auto Pair = Ctx.mkConcat(High, Low);
  const auto Flat = Ctx.mkConcat({Ctx.mkZero(8), High, Low});
  const auto Grouped = Ctx.mkConcat(Ctx.mkZExt(High, 16), Low);
  EXPECT_EQ(Flat, Ctx.mkZExt(Pair, 24));
  EXPECT_EQ(Grouped, Flat);
  EXPECT_EQ(Ctx.mkConcat(Ctx.mkZero(8), Grouped), Ctx.mkZExt(Pair, 32));
  const auto Interior = Ctx.mkConcat(High, Ctx.mkZExt(Low, 16));
  const auto Nonzero = Ctx.mkConcat(Ctx.mkConst(8, 0x93), Low);
  EXPECT_NE(Pair, Ctx.mkZExt(Low, 16));
  EXPECT_NE(Nonzero, Ctx.mkZExt(Low, 16));
  for (uint64_t A = 0; A != 256; ++A)
    for (uint64_t B = 0; B != 256; ++B) {
      const uint64_t Inputs[] = {A, B};
      EXPECT_EQ(Ctx.evalU64(Flat, Inputs), (A << 8) | B);
      EXPECT_EQ(Ctx.evalU64(Grouped, Inputs), (A << 8) | B);
      EXPECT_EQ(Ctx.evalU64(Interior, Inputs), (A << 16) | B);
      EXPECT_EQ(Ctx.evalU64(Nonzero, Inputs), 0x9300 | B);
    }
}

TEST(SymExpr, NonLowBitwiseWindowsRetainProvedConstants) {
  for (unsigned Width : {16u, 32u, 64u, 128u, 256u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width);
    const unsigned Low = Width - 8;
    const auto Mask = llvm::APInt::getBitsSet(Width, Low, Width);
    const auto Literal = llvm::APInt(Width, 0x5a).shl(Low);
    auto Slice = [&](SymRef Value) {
      return Ctx.constantWindow(Value, Low, 8);
    };
    EXPECT_EQ(Slice(Ctx.mkAnd(X, Ctx.mkConst(~Mask))),
              std::optional<llvm::APInt>(llvm::APInt(8, 0)));
    EXPECT_EQ(Slice(Ctx.mkOr(X, Ctx.mkConst(Mask))),
              std::optional<llvm::APInt>(llvm::APInt(8, 0xff)));
    EXPECT_EQ(Slice(Ctx.mkXor(Ctx.mkAnd(X, Ctx.mkConst(~Mask)),
                              Ctx.mkConst(Literal))),
              std::optional<llvm::APInt>(llvm::APInt(8, 0x5a)));
    EXPECT_EQ(Slice(Ctx.mkNot(Ctx.mkOr(X, Ctx.mkConst(Mask)))),
              std::optional<llvm::APInt>(llvm::APInt(8, 0)));
    EXPECT_FALSE(Slice(Ctx.mkXor(X, Ctx.mkConst(Literal))));
  }
}

TEST(SymExpr, ConstantWindowsFollowStructuralCarriersWithoutLosingUnknowns) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 8);
  SymRef Y = Ctx.mkVar("y", 8);
  SymRef Wide = Ctx.mkZExt(X, 32);
  EXPECT_EQ(Ctx.constantWindow(Ctx.mkNot(Wide), 8, 16),
            std::optional<llvm::APInt>(llvm::APInt(16, 0xffff)));
  SymRef Packed = Ctx.mkConcat(Ctx.mkConst(8, 0xa5), X);
  EXPECT_EQ(Ctx.constantWindow(Ctx.mkXor(Packed, Ctx.mkZExt(Y, 16)), 8, 8),
            std::optional<llvm::APInt>(llvm::APInt(8, 0xa5)));
  SymRef Crossed = Ctx.mkExtract(Ctx.mkNot(Wide), 4, 24);
  SymRef Mixed = Ctx.mkXor(Crossed, Ctx.mkZExt(Y, 24));
  EXPECT_EQ(Ctx.constantWindow(Mixed, 12, 8),
            std::optional<llvm::APInt>(llvm::APInt(8, 0xff)));

  const SymRef Unknown[] = {
      Ctx.mkExtract(Ctx.mkNot(Wide), 7, 2),
      Ctx.mkExtract(Ctx.mkNot(Ctx.mkSExt(X, 32)), 8, 8),
      Ctx.mkExtract(Ctx.mkXor(Packed, Ctx.mkZExt(Y, 16)), 4, 8)};
  for (SymRef Value : Unknown)
    EXPECT_FALSE(Ctx.constantWindow(Value, 0, Ctx.width(Value)));
  for (uint64_t A = 0; A != 256; ++A) {
    uint64_t Values[] = {A, A ^ 0x5a};
    EXPECT_EQ(Ctx.evalU64(Unknown[0], Values), ((~A) >> 7) & 3);
    EXPECT_EQ(Ctx.evalU64(Unknown[1], Values), A < 128 ? 0xffu : 0u);
    EXPECT_EQ(Ctx.evalU64(Unknown[2], Values),
              (((0xa500 | A) ^ (A ^ 0x5a)) >> 4) & 0xff);
  }
}

TEST(SymExpr, NonLowBitwiseWindowsMatchEverySmallInputAndMask) {
  for (unsigned M = 0; M != 16; ++M)
    for (unsigned N = 0; N != 16; ++N) {
      SymContext Ctx;
      SymRef X = Ctx.mkVar("x", 4), Y = Ctx.mkVar("y", 4);
      SymRef MX = Ctx.mkAnd(X, Ctx.mkConst(4, M));
      SymRef NY = Ctx.mkOr(Y, Ctx.mkConst(4, N));
      const SymRef Expressions[] = {Ctx.mkAnd(MX, NY), Ctx.mkOr(MX, NY),
                                    Ctx.mkXor(MX, NY), Ctx.mkNot(MX)};
      for (unsigned Low = 1; Low != 4; ++Low)
        for (unsigned Bits = 1; Bits <= 4 - Low; ++Bits) {
          const uint64_t Mask = (1u << Bits) - 1;
          llvm::SmallVector<std::optional<llvm::APInt>, 4> Windows;
          for (SymRef Expression : Expressions)
            Windows.push_back(Ctx.constantWindow(Expression, Low, Bits));
          for (uint64_t A = 0; A != 16; ++A)
            for (uint64_t B = 0; B != 16; ++B) {
              const uint64_t Expected[] = {(A & M) & (B | N), (A & M) | (B | N),
                                           (A & M) ^ (B | N), ~(A & M)};
              for (unsigned I = 0; I != std::size(Expected); ++I)
                if (Windows[I])
                  ASSERT_EQ(Windows[I]->getZExtValue(),
                            (Expected[I] >> Low) & Mask)
                      << A << ',' << B << ',' << M << ',' << N << ',' << Low
                      << ',' << Bits << ',' << I;
            }
        }
    }
}

TEST(SymExpr, ConstantWindowSearchDoesNotExpandDeepOrWideUnknownExpressions) {
  SymContext Ctx;
  SymRef Root = Ctx.mkVar("root", 64);
  for (unsigned I = 0; I != 96; ++I) {
    SymRef Other = Ctx.mkFreshVar(64, "other");
    Root = I % 2 ? Ctx.mkOr(Root, Other) : Ctx.mkAnd(Root, Other);
  }
  llvm::SmallVector<SymRef, 512> Wide;
  for (unsigned I = 0; I != 512; ++I)
    Wide.push_back(Ctx.mkFreshVar(64, "wide"));
  SymRef Many = Ctx.mkXor(Wide);
  for (SymRef Value : {Root, Many}) {
    const size_t Before = Ctx.numNodes();
    EXPECT_FALSE(Ctx.constantWindow(Value, 17, 13));
    EXPECT_EQ(Ctx.numNodes(), Before);
  }
}

TEST(SymExpr, ConstantWindowBudgetIncludesWideConstantsAndDepthRejections) {
  SymContext Ctx;
  constexpr unsigned Width = 64 * 512;
  SymRef Wide = Ctx.mkAnd(Ctx.mkVar("wide", Width),
                          Ctx.mkConst(llvm::APInt::getLowBitsSet(Width, 16)));
  // A small result must not authorize copying an arbitrarily wide constant.
  EXPECT_FALSE(Ctx.constantWindow(Wide, 20, 8));

  for (unsigned Count : {4u, 512u}) {
    SymContext Local;
    llvm::SmallVector<SymRef, 512> Operands;
    for (unsigned I = 0; I != Count; ++I)
      Operands.push_back(Local.mkFreshVar(64, "leaf"));
    SymRef Deep = Local.mkXor(Operands);
    // The wide node sits at depth 31, just before the recursion cutoff.
    for (unsigned I = 0; I != 30; ++I) {
      SymRef Other = Local.mkFreshVar(64, "other");
      Deep = I % 2 ? Local.mkAnd(Deep, Other) : Local.mkOr(Deep, Other);
    }
    SymRef Known = Local.mkNot(Local.mkZExt(Local.mkFreshVar(8, "low"), 64));
    SymRef Root = Local.mkOr(Deep, Known);
    ASSERT_EQ(Local.operand(Root, 0), Deep);
    const size_t Before = Local.numNodes();
    auto Window = Local.constantWindow(Root, 16, 8);
    if (Count == 4)
      EXPECT_EQ(Window, std::optional<llvm::APInt>(llvm::APInt(8, 0xff)));
    else
      EXPECT_FALSE(Window);
    EXPECT_EQ(Local.numNodes(), Before);
  }
}

TEST(SymExpr, ConstantWindowSharesActualWorkWithoutRelaxingQueryCeilings) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("byte", 8);
  SymRef Value = Ctx.mkZExt(X, 64);
  unsigned Remaining = 1000;
  const auto Expected = std::optional<llvm::APInt>(llvm::APInt(8, 0));
  const auto Before = Ctx.numNodes();
  EXPECT_EQ(Ctx.constantWindow(Value, 32, 8, Remaining), Expected);
  const unsigned Used = 1000 - Remaining;
  ASSERT_GT(Used, 0U);
  ASSERT_LT(Used, 256U);
  unsigned Exact = Used, Short = Used - 1, Zero = 0;
  EXPECT_EQ(Ctx.constantWindow(Value, 32, 8, Exact), Expected);
  EXPECT_EQ(Exact, 0U);
  EXPECT_FALSE(Ctx.constantWindow(Value, 32, 8, Short));
  EXPECT_FALSE(Ctx.constantWindow(Value, 32, 8, Zero));
  EXPECT_EQ(Ctx.constantWindow(Value, 32, 8, Remaining), Expected);
  EXPECT_EQ(Remaining, 1000 - 2 * Used);
  unsigned Unknown = 1000;
  EXPECT_FALSE(Ctx.constantWindow(Value, 0, 8, Unknown));
  EXPECT_LT(Unknown, 1000U);
  EXPECT_EQ(Ctx.numNodes(), Before);

  constexpr unsigned WideBits = 64 * 512;
  SymRef Wide = Ctx.mkConst(llvm::APInt::getLowBitsSet(WideBits, 16));
  unsigned Large = 10000;
  EXPECT_FALSE(Ctx.constantWindow(Wide, 20, 8, Large));
  EXPECT_GE(Large, 10000 - 256U);
}

TEST(SymExpr, ConstantWindowRejectsInvalidAndOversizedQueries) {
  SymContext Ctx;
  SymRef Value = Ctx.mkConst(128, 123);
  const size_t Before = Ctx.numNodes();
  EXPECT_FALSE(Ctx.constantWindow({}, 0, 8));
  EXPECT_FALSE(Ctx.constantWindow(SymRef(uint32_t(Before + 1)), 0, 8));
  EXPECT_FALSE(Ctx.constantWindow(Value, 0, 0));
  EXPECT_FALSE(Ctx.constantWindow(Value, 0, 65));
  EXPECT_FALSE(Ctx.constantWindow(Value, 127, 2));
  EXPECT_FALSE(Ctx.constantWindow(Value, UINT32_MAX, 8));
  EXPECT_EQ(Ctx.constantWindow(Value, 64, 64),
            std::optional<llvm::APInt>(llvm::APInt(64, 0)));
  EXPECT_EQ(Ctx.numNodes(), Before);
}

TEST(SymExpr, ConstantWindowsDoNotProjectAcrossArithmeticCarries) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 16);
  SymRef Sum = Ctx.mkAdd(X, Ctx.mkOne(16));
  SymRef Window = Ctx.mkExtract(Ctx.mkNot(Sum), 8, 8);
  EXPECT_FALSE(Ctx.constantWindow(Window, 0, 8));
  for (uint64_t A : {0u, 0xfeu, 0xffu, 0x100u, 0x7fffu, 0xffffu}) {
    uint64_t Values[] = {A};
    EXPECT_EQ(Ctx.evalU64(Window, Values), (~(A + 1) >> 8) & 0xff);
  }
}

TEST(SymExpr, ByteStateReassemblyPreservesMixedKnownAndUnknownWindows) {
  for (unsigned Width : {16u, 32u, 64u, 128u, 256u}) {
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width);
    const auto Mask = llvm::APInt::getSplat(Width, llvm::APInt(16, 0x00ff));
    const auto Fixed = llvm::APInt::getSplat(Width, llvm::APInt(16, 0xa500));
    SymRef Word = Ctx.mkOr(Ctx.mkAnd(X, Ctx.mkConst(Mask)), Ctx.mkConst(Fixed));
    llvm::SmallVector<SymRef, 32> Bytes;
    for (unsigned Low = Width; Low != 0; Low -= 8) {
      SymRef Byte = Ctx.mkExtract(Word, Low - 8, 8);
      if (Low % 16 == 0)
        EXPECT_EQ(Ctx.constantWindow(Byte, 0, 8),
                  std::optional<llvm::APInt>(llvm::APInt(8, 0xa5)));
      else
        EXPECT_FALSE(Ctx.constantWindow(Byte, 0, 8));
      Bytes.push_back(Byte);
    }
    SymRef Roundtrip = Ctx.mkConcat(Bytes);
    EXPECT_EQ(Roundtrip, Word);
    for (unsigned A = 0; A != 256; ++A) {
      const auto Value = llvm::APInt::getSplat(Width, llvm::APInt(8, A));
      llvm::APInt Values[] = {Value};
      EXPECT_EQ(Ctx.eval(Roundtrip, Values), (Value & Mask) | Fixed);
    }
  }
}

TEST(SymExpr, LowPrefixProjectsModularArithmeticAndBitwiseOperations) {
  for (uint32_t Width : {1u, 3u, 8u, 16u, 32u, 64u, 128u, 256u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width);
    SymRef Y = Ctx.mkVar("y", Width);
    SymRef WideX = Ctx.mkZExt(X, Width * 2);
    SymRef WideY = Ctx.mkSExt(Y, Width * 2);
    auto Low = [&](SymRef R) { return Ctx.mkExtract(R, 0, Width); };
    EXPECT_EQ(Low(Ctx.mkAdd(WideX, WideY)), Ctx.mkAdd(X, Y));
    EXPECT_EQ(Low(Ctx.mkSub(WideX, WideY)), Ctx.mkSub(X, Y));
    EXPECT_EQ(Low(Ctx.mkMul(WideX, WideY)), Ctx.mkMul(X, Y));
    EXPECT_EQ(Low(Ctx.mkAnd(WideX, WideY)), Ctx.mkAnd(X, Y));
    EXPECT_EQ(Low(Ctx.mkOr(WideX, WideY)), Ctx.mkOr(X, Y));
    EXPECT_EQ(Low(Ctx.mkXor(WideX, WideY)), Ctx.mkXor(X, Y));
    EXPECT_EQ(Low(Ctx.mkNot(WideX)), Ctx.mkNot(X));

    // Widening an unsigned all-ones word does not make it wide -1. Its low
    // product nevertheless implements modular negation at the original width.
    SymRef Coefficient =
        Ctx.mkConst(llvm::APInt::getAllOnes(Width).zext(Width * 2));
    EXPECT_EQ(Low(Ctx.mkMul(WideY, Coefficient)), Ctx.mkNeg(Y));
  }
}

TEST(SymExpr, ProjectedWordOperationsAgreeForEverySmallInputIncludingHighBits) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 4);
  SymRef Y = Ctx.mkVar("y", 4);
  const SymRef Wide[] = {Ctx.mkAdd(X, Y), Ctx.mkSub(X, Y), Ctx.mkMul(X, Y),
                         Ctx.mkAnd(X, Y), Ctx.mkOr(X, Y),  Ctx.mkXor(X, Y),
                         Ctx.mkNot(X)};
  for (unsigned Width : {1u, 2u, 3u}) {
    uint64_t Mask = (1u << Width) - 1;
    for (unsigned A = 0; A != 16; ++A) {
      for (unsigned B = 0; B != 16; ++B) {
        const uint64_t Expected[] = {A + B, uint64_t(A) - B, A * B,       A & B,
                                     A | B, A ^ B,           ~uint64_t(A)};
        uint64_t Values[] = {A, B};
        for (unsigned I = 0; I != std::size(Wide); ++I)
          EXPECT_EQ(Ctx.evalU64(Ctx.mkExtract(Wide[I], 0, Width), Values),
                    Expected[I] & Mask);
      }
    }
  }
}

TEST(SymExpr, ProjectionRetainsExtensionWhenItsInputIsNarrowerThanTheResult) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 3);
  SymRef Y = Ctx.mkVar("y", 4);
  SymRef SX = Ctx.mkSExt(X, 8);
  SymRef ZY = Ctx.mkZExt(Y, 8);
  const SymRef Wide[] = {Ctx.mkAdd(SX, ZY), Ctx.mkSub(SX, ZY),
                         Ctx.mkMul(SX, ZY), Ctx.mkAnd(SX, ZY),
                         Ctx.mkOr(SX, ZY),  Ctx.mkXor(SX, ZY)};
  for (unsigned A = 0; A != 8; ++A) {
    for (unsigned B = 0; B != 16; ++B) {
      const uint64_t SignedA = A < 4 ? A : uint64_t(A) - 8;
      const uint64_t Expected[] = {SignedA + B, SignedA - B, SignedA * B,
                                   SignedA & B, SignedA | B, SignedA ^ B};
      uint64_t Values[] = {A, B};
      for (unsigned I = 0; I != std::size(Wide); ++I)
        EXPECT_EQ(Ctx.evalU64(Ctx.mkExtract(Wide[I], 0, 5), Values),
                  Expected[I] & 31);
    }
  }
}

TEST(SymExpr, ProjectionPreservesHighProductsDivisionAndShiftAmounts) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 8);
  SymRef Y = Ctx.mkVar("y", 8);
  SymRef Product = Ctx.mkMul(X, Y);
  SymRef HighProduct = Ctx.mkExtract(Product, 4, 4);
  EXPECT_EQ(Ctx.op(HighProduct), SymOp::Extract);
  EXPECT_EQ(Ctx.operand(HighProduct, 0), Product);
  uint64_t ProductValues[] = {15, 15};
  EXPECT_EQ(Ctx.evalU64(HighProduct, ProductValues), 14u);

  SymRef Division = Ctx.mkUDiv(X, Y);
  SymRef LowDivision = Ctx.mkExtract(Division, 0, 4);
  EXPECT_EQ(Ctx.op(LowDivision), SymOp::Extract);
  uint64_t DivisionValues[] = {16, 3};
  EXPECT_EQ(Ctx.evalU64(LowDivision, DivisionValues), 5u);

  SymRef Shift = Ctx.mkShl(X, Y);
  SymRef LowShift = Ctx.mkExtract(Shift, 0, 4);
  EXPECT_EQ(Ctx.op(LowShift), SymOp::Extract);
  uint64_t ShiftValues[] = {1, 16};
  EXPECT_EQ(Ctx.evalU64(LowShift, ShiftValues), 0u);

  SymRef Right = Ctx.mkExtract(Ctx.mkLShr(X, Y), 0, 4);
  uint64_t RightValues[] = {16, 1};
  EXPECT_EQ(Ctx.evalU64(Right, RightValues), 8u);
}

TEST(SymExpr, LowPrefixWalkHandlesDeepSharedWordExpressionsIteratively) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 16);
  SymRef Y = Ctx.mkVar("y", 16);
  SymRef Root = X;
  uint64_t Expected = 173;
  for (unsigned I = 0; I != 10000; ++I) {
    Root = Ctx.mkXor(Ctx.mkAdd(Root, Y), Ctx.mkConst(16, 0x35));
    Expected = ((Expected + 29) ^ 0x35) & 0xff;
  }
  SymRef Low = Ctx.mkExtract(Root, 0, 8);
  EXPECT_EQ(Ctx.op(Low), SymOp::Xor);
  uint64_t Values[] = {173, 29};
  EXPECT_EQ(Ctx.evalU64(Low, Values), Expected);
}

TEST(SymExpr, ByteSlicesReassembleComputedWordsAfterLowPrefixProjection) {
  for (uint32_t Width : {16u, 32u, 64u, 128u, 256u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width);
    SymRef Y = Ctx.mkVar("y", Width);
    const SymRef Words[] = {Ctx.mkAdd(X, Y), Ctx.mkSub(X, Y), Ctx.mkMul(X, Y),
                            Ctx.mkAnd(X, Y), Ctx.mkOr(X, Y),  Ctx.mkXor(X, Y),
                            Ctx.mkNot(X)};
    for (SymRef Word : Words) {
      llvm::SmallVector<SymRef, 32> Bytes;
      for (uint32_t Low = Width; Low != 0; Low -= 8)
        Bytes.push_back(Ctx.mkExtract(Word, Low - 8, 8));
      EXPECT_EQ(Ctx.mkConcat(Bytes), Word);
      SymRef Prefix = Bytes.back();
      for (uint32_t Low = 8; Low < Width; Low += 8) {
        Prefix = Ctx.mkConcat(Ctx.mkExtract(Word, Low, 8), Prefix);
        EXPECT_EQ(Prefix, Ctx.mkExtract(Word, 0, Low + 8));
      }
      EXPECT_EQ(Prefix, Word);
      for (uint32_t Split : {1u, Width / 2, Width - 1})
        EXPECT_EQ(Ctx.mkConcat(Ctx.mkExtract(Word, Split, Width - Split),
                               Ctx.mkExtract(Word, 0, Split)),
                  Word);

      // Only the actual low prefix may reconcile with the upper slices.
      Bytes.back() = Ctx.mkXor(Bytes.back(), Ctx.mkOne(8));
      SymRef Different = Ctx.mkConcat(Bytes);
      EXPECT_NE(Different, Word);
      llvm::APInt Values[] = {llvm::APInt(Width, 0x18f),
                              llvm::APInt(Width, 0x175)};
      EXPECT_EQ(Ctx.eval(Different, Values),
                Ctx.eval(Word, Values) ^ llvm::APInt(Width, 1));
    }
  }
}

TEST(SymExpr, SelectFoldsToItsConditionWhenTheArmsAreTheTruthValues) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef C = Ctx.mkUlt(X, Ctx.mkConst(W32, 10));

  EXPECT_EQ(Ctx.mkIte(C, Ctx.mkTrue(), Ctx.mkFalse()), C);
  EXPECT_EQ(Ctx.mkIte(C, Ctx.mkFalse(), Ctx.mkTrue()), Ctx.mkNot(C));
  EXPECT_EQ(Ctx.mkIte(Ctx.mkTrue(), X, Ctx.mkZero(W32)), X);
  EXPECT_EQ(Ctx.mkIte(C, X, X), X);
}

TEST(SymExpr, DagSizeCountsSharedSubtermsOnce) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);
  SymRef Shared = Ctx.mkXor(X, Y);

  // Doubling a shared node adds the product and its coefficient, not a second
  // copy of the subtree.  A tree-shaped count would call this expression twice
  // the size it costs to work with.
  size_t Before = Ctx.dagSize(Shared);
  size_t After = Ctx.dagSize(Ctx.mkAdd(Shared, Shared));
  EXPECT_EQ(Before, 3u);
  EXPECT_EQ(After, Before + 2);
}

TEST(SymExpr, ReadabilityCostCountsEveryPrintedUse) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);
  SymRef Z = Ctx.mkVar("z", W32);
  SymRef W = Ctx.mkVar("w", W32);
  SymRef Shared = Ctx.mkXor(X, Y);

  // Populate the cache before appending the expressions below: a later query
  // must extend it rather than recompute or return a stale prefix.
  EXPECT_EQ(Ctx.readabilityCost(Shared), 3u);

  SymRef Twice = Ctx.mkOr(Ctx.mkAnd(Shared, Z), Ctx.mkAnd(Shared, W));
  EXPECT_EQ(Ctx.dagSize(Twice), 8u);
  EXPECT_EQ(Ctx.readabilityCost(Twice), 11u);

  // The all-ones factor is how the graph spells a sign, so it is not a printed
  // quantity in the cost model.
  EXPECT_EQ(Ctx.readabilityCost(Ctx.mkNeg(Shared)), 4u);
}

TEST(SymExpr, CollectVarsReportsEveryReachableVariableOnce) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);
  SymRef E = Ctx.mkAdd(Ctx.mkXor(X, Y), Ctx.mkMul(X, Ctx.mkConst(W32, 3)));

  llvm::SmallVector<uint32_t, 4> Vars;
  Ctx.collectVars(E, Vars);
  ASSERT_EQ(Vars.size(), 2u);
  EXPECT_EQ(Ctx.varInfo(Vars[0]).Name, "x");
  EXPECT_EQ(Ctx.varInfo(Vars[1]).Name, "y");
}

TEST(SymExpr, FindVarSeesDeclarationsAndNothingElse) {
  SymContext Ctx;
  SymRef X32 = Ctx.mkVar("x", W32);
  EXPECT_TRUE(Ctx.hasVarName("x"));
  EXPECT_FALSE(Ctx.hasVarName("nope"));
  ASSERT_TRUE(Ctx.findVar("x").has_value());
  EXPECT_EQ(*Ctx.findVar("x"), Ctx.varId(X32));
  EXPECT_EQ(Ctx.varInfo(*Ctx.findVar("x")).Width, W32);
  EXPECT_FALSE(Ctx.findVar("nope").has_value());

  Ctx.mkVar("x", 8);
  EXPECT_TRUE(Ctx.hasVarName("x"));
  EXPECT_FALSE(Ctx.findVar("x").has_value());
}

TEST(SymExpr, FreshVariablesCarryExplicitOrigin) {
  SymContext Ctx;
  SymRef Input = Ctx.mkVar("reg$0", W32);
  SymRef Havoc = Ctx.mkFreshVar(W32, "reg$0$");

  EXPECT_FALSE(Ctx.varInfo(Ctx.varId(Input)).Fresh);
  EXPECT_TRUE(Ctx.varInfo(Ctx.varId(Havoc)).Fresh);
}

TEST(SymExpr, StructuredInputNeverMutatesAPlainVariableIdentity) {
  SymContext Ctx;
  const SymInputOrigin Origin{SymInputKind::Register, 24, 4, 0};

  SymRef Plain = Ctx.mkVar("reg$24", W32);
  SymRef Input = Ctx.mkInputVar("reg$24", W32, Origin);
  EXPECT_NE(Input, Plain);
  EXPECT_EQ(Plain, Ctx.mkVar("reg$24", W32));
  EXPECT_FALSE(Ctx.varInfo(Ctx.varId(Plain)).InputOrigin.has_value());
  EXPECT_EQ(Input, Ctx.varRef(Ctx.varId(Input)));
  EXPECT_TRUE(Ctx.hasVarName("reg$24"));
  EXPECT_FALSE(Ctx.findVar("reg$24").has_value());
  EXPECT_FALSE(Ctx.varRef(Ctx.numVars()).isValid());
  EXPECT_FALSE(Ctx.mkInputVar("zero-width", 0,
                              SymInputOrigin{SymInputKind::Register, 0, 1, 0})
                   .isValid());

  const SymVarInfo &Info = Ctx.varInfo(Ctx.varId(Input));
  ASSERT_TRUE(Info.InputOrigin.has_value());
  EXPECT_EQ(*Info.InputOrigin, Origin);
  EXPECT_FALSE(Info.Fresh);

  SymContext ReverseCtx;
  SymRef InputFirst = ReverseCtx.mkInputVar("reg$24", W32, Origin);
  SymRef PlainAfter = ReverseCtx.mkVar("reg$24", W32);
  EXPECT_NE(PlainAfter, InputFirst);
  EXPECT_FALSE(
      ReverseCtx.varInfo(ReverseCtx.varId(PlainAfter)).InputOrigin.has_value());
}

TEST(SymExpr, SameInputNameAndWidthKeepsDifferentOriginsIndependent) {
  SymContext Ctx;
  const SymInputOrigin RegisterOrigin{SymInputKind::Register, 24, 4, 0};
  const SymInputOrigin TemporaryOrigin{SymInputKind::Temporary, 24, 4, 0};

  SymRef Register = Ctx.mkInputVar("machine-input", W32, RegisterOrigin);
  SymRef Temporary = Ctx.mkInputVar("machine-input", W32, TemporaryOrigin);

  EXPECT_NE(Register, Temporary);
  EXPECT_NE(Ctx.varId(Register), Ctx.varId(Temporary));
  EXPECT_EQ(Ctx.varInfo(Ctx.varId(Register)).InputOrigin, RegisterOrigin);
  EXPECT_EQ(Ctx.varInfo(Ctx.varId(Temporary)).InputOrigin, TemporaryOrigin);
  EXPECT_TRUE(Ctx.hasVarName("machine-input"));
  EXPECT_FALSE(Ctx.findVar("machine-input").has_value());
}

TEST(SymExpr, OneNameAtDifferentWidthsHasDistinctConsistentVariables) {
  SymContext Ctx;
  SymRef Byte = Ctx.mkVar("same-name", 8);
  SymRef Word = Ctx.mkVar("same-name", 16);

  EXPECT_NE(Ctx.varId(Byte), Ctx.varId(Word));
  EXPECT_EQ(Ctx.varInfo(Ctx.varId(Byte)).Width, Ctx.width(Byte));
  EXPECT_EQ(Ctx.varInfo(Ctx.varId(Word)).Width, Ctx.width(Word));
  EXPECT_EQ(Byte, Ctx.mkVar("same-name", 8));
  EXPECT_EQ(Word, Ctx.mkVar("same-name", 16));
}

TEST(SymExpr, OneInputNameAtDifferentWidthsKeepsDistinctMetadata) {
  SymContext Ctx;
  const SymInputOrigin ByteOrigin{SymInputKind::Register, 0, 1, 0};
  const SymInputOrigin WordOrigin{SymInputKind::Register, 16, 2, 0};
  SymRef Byte = Ctx.mkInputVar("same-input", 8, ByteOrigin);
  SymRef Word = Ctx.mkInputVar("same-input", 16, WordOrigin);

  ASSERT_TRUE(Ctx.isVar(Byte));
  ASSERT_TRUE(Ctx.isVar(Word));
  EXPECT_NE(Ctx.varId(Byte), Ctx.varId(Word));
  EXPECT_EQ(Ctx.varInfo(Ctx.varId(Byte)).Width, Ctx.width(Byte));
  EXPECT_EQ(Ctx.varInfo(Ctx.varId(Word)).Width, Ctx.width(Word));
  EXPECT_EQ(Ctx.varInfo(Ctx.varId(Byte)).InputOrigin, ByteOrigin);
  EXPECT_EQ(Ctx.varInfo(Ctx.varId(Word)).InputOrigin, WordOrigin);
}

TEST(SymExpr, SubstitutionRebuildsThroughTheCanonicalisingBuilders) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);

  // Replacing y by x turns `x + y` into `x + x`, which the builders collect
  // into `2*x` rather than leaving as a sum of equal terms.
  SymRef Sum = Ctx.mkAdd(X, Y);
  EXPECT_EQ(Ctx.substituteVar(Sum, Ctx.varId(Y), X),
            Ctx.mkMul(Ctx.mkConst(W32, 2), X));
  EXPECT_EQ(Ctx.substituteVar(Sum, Ctx.varId(Y), Ctx.mkZero(W32)), X);
}

//===----------------------------------------------------------------------===//
// Evaluation
//===----------------------------------------------------------------------===//

TEST(SymExpr, EvaluationAgreesAcrossAllThreeEntryPoints) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);
  // (x ^ y) + 2*(x & y), which is x + y written the long way round.
  SymRef E = Ctx.mkAdd(Ctx.mkXor(X, Y),
                       Ctx.mkMul(Ctx.mkConst(W32, 2), Ctx.mkAnd(X, Y)));
  ASSERT_TRUE(Ctx.fitsU64(E));

  SymEvalPlan Plan(Ctx, E);
  ASSERT_TRUE(Plan.fitsU64());

  std::mt19937_64 Rng(20260812);
  for (unsigned I = 0; I < 512; ++I) {
    uint64_t A = Rng() & 0xFFFFFFFFu;
    uint64_t B = Rng() & 0xFFFFFFFFu;
    uint64_t Want = (A + B) & 0xFFFFFFFFu;

    std::vector<uint64_t> VarsU64(Ctx.numVars(), 0);
    VarsU64[Ctx.varId(X)] = A;
    VarsU64[Ctx.varId(Y)] = B;

    std::vector<llvm::APInt> VarsAP(Ctx.numVars(), llvm::APInt(W32, 0));
    VarsAP[Ctx.varId(X)] = llvm::APInt(W32, A);
    VarsAP[Ctx.varId(Y)] = llvm::APInt(W32, B);

    EXPECT_EQ(Ctx.evalU64(E, VarsU64), Want);
    EXPECT_EQ(Ctx.eval(E, VarsAP).getZExtValue(), Want);
    EXPECT_EQ(Plan.evalU64(VarsU64), Want);
    EXPECT_EQ(Plan.eval(VarsAP).getZExtValue(), Want);
  }
}

TEST(SymExpr, EvaluationPlanReportsTheVariablesItReads) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  Ctx.mkVar("unused", W32);
  SymRef Y = Ctx.mkVar("y", W32);

  SymEvalPlan Plan(Ctx, Ctx.mkAnd(X, Y));
  ASSERT_EQ(Plan.vars().size(), 2u);
  EXPECT_EQ(Plan.vars()[0], Ctx.varId(X));
  EXPECT_EQ(Plan.vars()[1], Ctx.varId(Y));
  EXPECT_GT(Plan.numSteps(), 0u);
}

TEST(SymExpr, U64ArithmeticShiftByZeroKeepsTheWholeWord) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 64);
  SymRef Amount = Ctx.mkVar("amount", 64);
  SymEvalPlan Plan(Ctx, Ctx.mkAShr(X, Amount));
  ASSERT_TRUE(Plan.fitsU64());

  std::vector<uint64_t> Vars(Ctx.numVars(), 0);
  Vars[Ctx.varId(X)] = 0x8000000000000001ull;
  Vars[Ctx.varId(Amount)] = 0;
  EXPECT_EQ(Plan.evalU64(Vars), 0x8000000000000001ull);
}

TEST(SymExpr, DivisionAndRemainderFollowTheBitvectorTheory) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  std::vector<llvm::APInt> V{llvm::APInt(W32, 7)};

  // Totalised the way QF_BV totalises them, so a solver bridge needs no
  // correction terms: udiv by zero is all-ones, and the remainder is x.
  EXPECT_EQ(Ctx.eval(Ctx.mkUDiv(X, Ctx.mkZero(W32)), V),
            llvm::APInt::getAllOnes(W32));
  EXPECT_EQ(Ctx.eval(Ctx.mkURem(X, Ctx.mkZero(W32)), V), llvm::APInt(W32, 7));
  EXPECT_EQ(Ctx.eval(Ctx.mkUDiv(X, Ctx.mkConst(W32, 2)), V),
            llvm::APInt(W32, 3));
}

TEST(SymExpr, AWordWiderThanSixtyFourBitsIsOrdinary) {
  SymContext Ctx;
  constexpr uint32_t W256 = 256;
  SymRef X = Ctx.mkVar("x", W256);

  // Everything the canonicaliser does at 32 bits it does at 256, which is what
  // makes an EVM word a first-class citizen rather than a special case.
  EXPECT_EQ(Ctx.mkAdd(X, X), Ctx.mkMul(Ctx.mkConst(W256, 2), X));
  EXPECT_EQ(Ctx.mkXor(X, Ctx.mkOnes(W256)), Ctx.mkNot(X));
  EXPECT_EQ(Ctx.mkSub(X, X), Ctx.mkZero(W256));

  // `~x + 1` and `-x` are the same value but deliberately not the same node:
  // Not stays primitive because the MBA solver needs it as a generator of the
  // bitwise algebra.  Bridging the two is the simplifier's job.
  SymRef E = Ctx.mkAdd(Ctx.mkNot(X), Ctx.mkConst(W256, 1));
  EXPECT_NE(E, Ctx.mkNeg(X));
  EXPECT_FALSE(Ctx.fitsU64(E));

  llvm::APInt Big = llvm::APInt(W256, 1).shl(200) + 12345;
  std::vector<llvm::APInt> V(Ctx.numVars(), llvm::APInt(W256, 0));
  V[Ctx.varId(X)] = Big;
  EXPECT_EQ(Ctx.eval(E, V), -Big);
  EXPECT_EQ(SymEvalPlan(Ctx, E).eval(V), -Big);
}

} // namespace
