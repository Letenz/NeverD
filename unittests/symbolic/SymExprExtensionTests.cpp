//===- SymExprExtensionTests.cpp - Width-preserving bitwise rules ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymExpr.h"

#include <array>

using namespace neverd::symbolic;

TEST(SymExprExtension, BitwiseContractionPreservesHighConstantBits) {
  for (unsigned Width : {1U, 3U, 8U, 32U, 64U, 128U, 256U}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    const unsigned Wide = Width * 2;
    auto ZX = C.mkZExt(X, Wide), ZY = C.mkZExt(Y, Wide);
    auto Low = llvm::APInt::getLowBitsSet(Width, (Width + 1) / 2);
    auto WideLow = Low.zext(Wide);
    auto High = WideLow | llvm::APInt::getSignMask(Wide);
    EXPECT_EQ(C.mkAnd({ZX, ZY, C.mkConst(High)}),
              C.mkZExt(C.mkAnd({X, Y, C.mkConst(Low)}), Wide));
    EXPECT_EQ(C.mkOr({ZX, ZY, C.mkConst(WideLow)}),
              C.mkZExt(C.mkOr({X, Y, C.mkConst(Low)}), Wide));
    EXPECT_EQ(C.mkXor({ZX, ZY, C.mkConst(WideLow)}),
              C.mkZExt(C.mkXor({X, Y, C.mkConst(Low)}), Wide));
    auto HighOr = C.mkOr({ZX, ZY, C.mkConst(High)});
    auto HighXor = C.mkXor({ZX, ZY, C.mkConst(High)});
    auto Mixed = C.mkAnd(ZX, C.mkSExt(Y, Wide));
    for (const auto &A : {llvm::APInt(Width, 0), Low, ~Low})
      for (const auto &B : {llvm::APInt(Width, 0), Low, ~Low}) {
        EXPECT_EQ(C.eval(HighOr, {A, B}), A.zext(Wide) | B.zext(Wide) | High);
        EXPECT_EQ(C.eval(HighXor, {A, B}), A.zext(Wide) ^ B.zext(Wide) ^ High);
        EXPECT_EQ(C.eval(Mixed, {A, B}), A.zext(Wide) & B.sext(Wide));
      }
  }
}

TEST(SymExprExtension, LogicalShiftRetainsTheFullCountAcrossZeroExtension) {
  for (unsigned Width : {1U, 3U, 8U, 32U, 64U, 128U}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Count = C.mkVar("count", 256);
    auto Shift = C.mkLShr(C.mkZExt(X, Width * 2), Count);
    EXPECT_EQ(Shift, C.mkZExt(C.mkLShr(X, Count), Width * 2));
    for (const auto &N : {llvm::APInt(256, 0), llvm::APInt(256, Width - 1),
                          llvm::APInt(256, Width), llvm::APInt(256, 256),
                          llvm::APInt::getOneBitSet(256, 200)}) {
      auto A = llvm::APInt::getAllOnes(Width);
      auto Expected =
          N.uge(Width) ? llvm::APInt(Width, 0) : A.lshr(N.getZExtValue());
      EXPECT_EQ(C.eval(Shift, {A, N}), Expected.zext(Width * 2));
    }
  }
}

TEST(SymExprExtension, ZeroExtendedBooleanHasNoOutOfRangeValue) {
  SymContext C;
  const auto True = C.mkTrue(), False = C.mkFalse();
  auto Value = C.mkVar("value", 64), Divisor = C.mkVar("divisor", 64);
  for (unsigned I = 0; I != 4096; ++I)
    Value = C.mkUDiv(Value, Divisor);
  const auto Boolean = C.mkZExt(C.mkNot(C.mkEq(Value, C.mkZero(64))), 8);
  const auto One = C.mkConst(8, 1);
  const auto Before = C.numNodes();
  EXPECT_EQ(C.mkUlt(One, Boolean), False);
  EXPECT_EQ(C.mkUle(Boolean, One), True);
  EXPECT_EQ(C.numNodes(), Before);
}

TEST(SymExprExtension, UnsignedExtensionComparisonsMatchExhaustiveHostOracle) {
  for (unsigned Narrow = 1; Narrow <= 4; ++Narrow)
    for (unsigned Wide = Narrow + 1; Wide <= 8; ++Wide) {
      SymContext C;
      const auto X = C.mkVar("x", Narrow), Z = C.mkZExt(X, Wide);
      for (unsigned Number = 0; Number != (1U << Wide); ++Number) {
        const auto K = C.mkConst(Wide, Number);
        const std::array<SymRef, 4> Questions{C.mkUlt(K, Z), C.mkUlt(Z, K),
                                              C.mkUle(K, Z), C.mkUle(Z, K)};
        for (unsigned Value = 0; Value != (1U << Narrow); ++Value) {
          const std::array<bool, 4> Expected{Number < Value, Value < Number,
                                             Number <= Value, Value <= Number};
          for (unsigned I = 0; I != Questions.size(); ++I)
            EXPECT_EQ(
                C.eval(Questions[I], {llvm::APInt(Narrow, Value)}).isOne(),
                Expected[I]);
        }
      }
    }
}

TEST(SymExprExtension, ZeroExtensionMaximumKeepsStrictAndInclusiveBoundaries) {
  for (unsigned Narrow : {1U, 3U, 8U, 31U, 32U, 63U, 64U, 65U, 127U, 128U, 255U,
                          256U, 511U, 512U}) {
    SCOPED_TRACE(Narrow);
    const unsigned Wide = Narrow * 2;
    SymContext C;
    const auto X = C.mkVar("x", Narrow), Z = C.mkZExt(X, Wide);
    const auto Maximum = llvm::APInt::getLowBitsSet(Wide, Narrow);
    const auto At = C.mkConst(Maximum), Above = C.mkConst(Maximum + 1);
    EXPECT_EQ(C.mkUlt(At, Z), C.mkFalse());
    EXPECT_EQ(C.mkUle(Z, At), C.mkTrue());
    EXPECT_EQ(C.mkUlt(Z, Above), C.mkTrue());
    EXPECT_EQ(C.mkUle(Above, Z), C.mkFalse());
    const auto BelowMaximum = C.mkUlt(Z, At), AtMaximum = C.mkUle(At, Z);
    EXPECT_FALSE(C.isConst(BelowMaximum));
    EXPECT_FALSE(C.isConst(AtMaximum));
    for (const auto &Value :
         {llvm::APInt(Narrow, 0), llvm::APInt(Narrow, 1),
          llvm::APInt::getAllOnes(Narrow), llvm::APInt::getAllOnes(Narrow) - 1,
          llvm::APInt::getSignMask(Narrow)}) {
      EXPECT_EQ(C.eval(BelowMaximum, {Value}).isOne(),
                Value.zext(Wide).ult(Maximum));
      EXPECT_EQ(C.eval(AtMaximum, {Value}).isOne(),
                Maximum.ule(Value.zext(Wide)));
    }
  }
}

TEST(SymExprExtension, SignedExtensionsAndSignedOrderingKeepTheirMeaning) {
  for (unsigned Narrow = 1; Narrow <= 4; ++Narrow) {
    SymContext C;
    const auto X = C.mkVar("x", Narrow);
    const auto S = C.mkSExt(X, 8), Z = C.mkZExt(X, 8);
    const auto One = C.mkConst(8, 1), Negative = C.mkConst(8, 128);
    const auto Unsigned = C.mkUlt(One, S);
    const auto Signed = C.mkSlt(S, C.mkZero(8));
    const auto SignedLower = C.mkSlt(Negative, Z);
    const auto SignedUpper = C.mkSle(Z, Negative);
    for (unsigned Value = 0; Value != (1U << Narrow); ++Value) {
      const int SignedValue = Value & (1U << (Narrow - 1))
                                  ? int(Value) - int(1U << Narrow)
                                  : int(Value);
      const auto Input = llvm::APInt(Narrow, Value);
      EXPECT_EQ(C.eval(Unsigned, {Input}).isOne(),
                1U < (unsigned(SignedValue) & 255U));
      EXPECT_EQ(C.eval(Signed, {Input}).isOne(), SignedValue < 0);
      EXPECT_TRUE(C.eval(SignedLower, {Input}).isOne());
      EXPECT_FALSE(C.eval(SignedUpper, {Input}).isOne());
    }
  }
}

TEST(SymExprExtension, OptionalConstantInspectionStopsAtItsWordBound) {
  for (unsigned Wide : {4096U, 4097U}) {
    SymContext C;
    const auto X = C.mkVar("bit", 1), Z = C.mkZExt(X, Wide);
    const auto One = C.mkConst(Wide, 1);
    const auto Greater = C.mkUlt(One, Z), Within = C.mkUle(Z, One);
    EXPECT_EQ(C.isConst(Greater), Wide == 4096);
    EXPECT_EQ(C.isConst(Within), Wide == 4096);
    for (unsigned Value : {0U, 1U}) {
      EXPECT_FALSE(C.eval(Greater, {llvm::APInt(1, Value)}).isOne());
      EXPECT_TRUE(C.eval(Within, {llvm::APInt(1, Value)}).isOne());
    }
  }
}
