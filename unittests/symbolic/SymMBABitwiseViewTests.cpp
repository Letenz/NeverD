//===- SymMBABitwiseViewTests.cpp - Weighted unary bitwise views
//-----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check full-width reconstruction, independent proofs and bounded refusal.

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/solver/SymSynthVerifier.h"

#include <random>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

llvm::APInt randomViewWord(unsigned Width, std::mt19937_64 &Random) {
  llvm::APInt Value(Width, 0);
  for (unsigned Bit = 0; Bit < Width; Bit += 64)
    Value |= llvm::APInt(Width, Random()).shl(Bit);
  return Value;
}

struct ViewFixture {
  SymRef Input, Expected;
  llvm::APInt Key;
};

ViewFixture flippedPartition(SymContext &C, SymRef Carrier, int Weight,
                             unsigned Count, bool Compensate = false) {
  const unsigned Width = C.width(Carrier);
  std::mt19937_64 Random(0x195271 + Width);
  std::vector<llvm::APInt> Masks(Count, llvm::APInt(Width, 0));
  for (unsigned Bit = 0; Bit != Width; ++Bit)
    Masks[Random() % Count].setBit(Bit);
  llvm::APInt Key(Width, 0), Scale(Width, Weight, true);
  llvm::SmallVector<SymRef, 32> Terms;
  for (const auto &Mask : Masks) {
    auto Flip = randomViewWord(Width, Random);
    Key |= Flip & Mask;
    auto View = C.mkAnd(C.mkXor(Carrier, C.mkConst(Flip)), C.mkConst(Mask));
    Terms.push_back(C.mkMul(C.mkConst(Scale), View));
    if (Compensate) {
      Terms.push_back(C.mkMul(C.mkConst(Scale + Scale),
                              C.mkAnd(Carrier, C.mkConst(Flip & Mask))));
      Terms.push_back(C.mkConst(-Scale * (Flip & Mask)));
    }
  }
  if (Compensate)
    Key.clearAllBits();
  return {C.mkAdd(Terms),
          C.mkMul(C.mkConst(Scale), C.mkXor(Carrier, C.mkConst(Key))), Key};
}

TEST(SymMBABitwiseView, ReassemblesFlippedPartitionsAcrossFullWordWidths) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    for (auto Carrier :
         {C.mkXor(X, Y), C.mkAdd(X, Y), C.mkMul(X, Y),
          C.mkXor(C.mkAdd(X, Y), C.mkMul(X, Y)), C.mkAnd(X, Y), C.mkOr(X, Y)}) {
      SCOPED_TRACE(C.toString(Carrier));
      for (int Weight : {37, -11, 182}) {
        for (bool Compensate : {false, true}) {
          SCOPED_TRACE(Weight);
          SCOPED_TRACE(Compensate);
          auto F = flippedPartition(C, Carrier, Weight, 16, Compensate);
          detail::WorkBudget Budget(1 << 20);
          detail::SolveReport Report;
          auto Result =
              detail::solveWeightedMasks(C, F.Input, {}, Budget, Report);
          EXPECT_LT(C.readabilityCost(Result), C.readabilityCost(F.Input));
          EXPECT_LE(C.readabilityCost(Result), C.readabilityCost(F.Expected));
          EXPECT_EQ(Report.Evidence, MBAEvidence::Derivation);
          detail::WorkBudget Proof(1 << 20);
          EXPECT_TRUE(detail::proveMaskedSumIdentity(C, F.Input, Result,
                                                     Carrier, Proof));
          std::mt19937_64 Random(0x758914);
          for (unsigned Sample = 0; Sample != 64; ++Sample) {
            std::vector<llvm::APInt> Values{randomViewWord(Width, Random),
                                            randomViewWord(Width, Random)};
            EXPECT_EQ(C.eval(Result, Values), C.eval(F.Expected, Values));
          }
        }
      }
    }
  }
}

TEST(SymMBABitwiseView, NestedViewsAndNegatedGroupsPreserveOpaqueRemainders) {
  for (unsigned Width : {8u, 32u, 64u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto Carrier = C.mkXor(C.mkAdd(X, Y), C.mkMul(X, Y));
    std::mt19937_64 Random(0x521983);
    llvm::SmallVector<SymRef, 16> Positive, Negative;
    for (unsigned I = 0; I != 8; ++I) {
      auto A = C.mkConst(randomViewWord(Width, Random));
      auto B = C.mkConst(randomViewWord(Width, Random));
      auto Flipped = C.mkNot(C.mkXor(Carrier, A));
      Positive.push_back(C.mkOr(Flipped, B));
      Positive.push_back(C.mkAnd(Flipped, B));
      // (~(c^a)) + 2*(c&~a) - ~a == c.
      Positive.push_back(C.mkMul(C.mkConst(llvm::APInt(Width, 2)),
                                 C.mkAnd(Carrier, C.mkNot(A))));
      Negative.push_back(B);
      Negative.push_back(C.mkNot(A));
    }
    auto Grouped = C.mkSub(C.mkAdd(Positive), C.mkAdd(Negative));
    auto Remainder = C.mkXor(X, C.mkLShr(Y, C.mkConst(llvm::APInt(Width, 3))));
    auto Scale = C.mkConst(llvm::APInt(Width, -5, true));
    auto Input = C.mkAdd(C.mkMul(Scale, Grouped), Remainder);
    auto Expected = C.mkAdd(
        C.mkMul(C.mkConst(llvm::APInt(Width, -40, true)), Carrier), Remainder);
    detail::WorkBudget Budget(1 << 20);
    detail::SolveReport Report;
    auto Result = detail::solveWeightedMasks(C, Input, {}, Budget, Report);
    EXPECT_EQ(Result, Expected) << C.toString(Result);
    EXPECT_EQ(Report.Evidence, MBAEvidence::Derivation);
  }
}

TEST(SymMBABitwiseView, ExhaustiveBytePairsUseIndependentArithmeticOracle) {
  for (bool Compensate : {false, true}) {
    SymContext C;
    auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
    auto Carrier = C.mkXor(C.mkAdd(X, Y), C.mkMul(X, Y));
    auto F = flippedPartition(C, Carrier, -11, 16, Compensate);
    MBAOptions Options;
    Options.VerifySamples = 0;
    auto Result = simplifyMBADeep(C, F.Input, Options);
    ASSERT_LE(Result.SizeAfter, C.readabilityCost(F.Expected));
    for (unsigned A = 0; A != 256; ++A)
      for (unsigned B = 0; B != 256; ++B) {
        const unsigned Word = ((A + B) ^ (A * B)) & 255;
        const unsigned Want = (-11u * (Word ^ F.Key.getZExtValue())) & 255;
        EXPECT_EQ(C.evalU64(Result.Expr, {A, B}), Want);
      }
  }
}

TEST(SymMBABitwiseView, RandomMixedViewsPreserveEveryByteInput) {
  std::mt19937_64 Random(0x711542);
  for (unsigned Case = 0; Case != 48; ++Case) {
    SCOPED_TRACE(Case);
    SymContext C;
    auto X = C.mkVar("x", 8);
    llvm::SmallVector<SymRef, 16> Terms;
    for (unsigned I = 0; I != 12; ++I) {
      auto View = X;
      for (unsigned Depth = 0; Depth != 6; ++Depth) {
        auto K = C.mkConst(randomViewWord(8, Random));
        switch (Random() % 4) {
        case 0:
          View = C.mkAnd(View, K);
          break;
        case 1:
          View = C.mkOr(View, K);
          break;
        case 2:
          View = C.mkXor(View, K);
          break;
        case 3:
          View = C.mkNot(View);
          break;
        }
      }
      Terms.push_back(C.mkMul(C.mkConst(randomViewWord(8, Random)), View));
    }
    auto Input = C.mkAdd(Terms);
    auto Result = simplifyMBADeep(C, Input);
    for (unsigned Value = 0; Value != 256; ++Value)
      ASSERT_EQ(C.evalU64(Input, {Value}), C.evalU64(Result.Expr, {Value}));
  }
}

TEST(SymMBABitwiseView, ProofRejectsHighBitErrorsCarriesAndDifferentWords) {
  for (unsigned Width : {8u, 32u, 64u, 257u}) {
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto F = flippedPartition(C, X, 37, 16);
    auto Top = C.mkConst(llvm::APInt::getOneBitSet(Width, Width - 1));
    for (auto Wrong :
         {C.mkXor(F.Expected, Top),
          C.mkMul(C.mkConst(llvm::APInt(Width, 37)),
                  C.mkXor(Y, C.mkConst(F.Key))),
          C.mkAdd(F.Expected, C.mkMul(X, C.mkAdd(X, C.mkOne(Width))))}) {
      detail::WorkBudget Proof(1 << 20);
      EXPECT_FALSE(detail::proveMaskedSumIdentity(C, F.Input, Wrong, X, Proof));
    }
    // At x=0 and x=all-ones this nonlinear difference is zero. The pointwise
    // grammar, rather than those assignments alone, must reject it.
    auto Nonlinear = C.mkMul(X, C.mkAdd(X, C.mkOne(Width)));
    detail::WorkBudget Proof(1 << 20);
    EXPECT_FALSE(detail::proveMaskedSumIdentity(C, Nonlinear, C.mkZero(Width),
                                                X, Proof));
  }
}

TEST(SymMBABitwiseView, SmallBudgetCompletionCacheAndIncompleteProofRefusal) {
  SymContext C;
  auto X = C.mkVar("x", 64), Y = C.mkVar("y", 64);
  auto Carrier = C.mkXor(X, Y);
  auto F = flippedPartition(C, Carrier, 37, 16);
  MBAOptions Options;
  Options.MaxWork = 16384;
  Options.VerifySamples = 0;
  MBARewriteCache Cache(C, 128);
  auto Result = simplifyMBADeep(C, F.Input, Options, Cache);
  EXPECT_EQ(Result.Expr, F.Expected);
  EXPECT_LT(Result.Work, Options.MaxWork);
  EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
  EXPECT_EQ(simplifyMBADeep(C, F.Input, Options, Cache).Work, 1u);
  for (size_t Work : {0u, 1u, 64u, 256u, 1024u}) {
    detail::WorkBudget Budget(Work);
    detail::SolveReport Report;
    EXPECT_EQ(detail::solveWeightedMasks(C, F.Input, {}, Budget, Report),
              F.Input);
    EXPECT_LE(Budget.used(), Work);
    detail::WorkBudget Proof(Work);
    EXPECT_FALSE(
        detail::proveMaskedSumIdentity(C, F.Input, F.Expected, Carrier, Proof));
  }
  for (size_t Bytes : {0u, 64u, 4096u}) {
    Options.MaxTableBytes = Bytes;
    detail::WorkBudget Budget(1 << 20);
    detail::SolveReport Report;
    EXPECT_EQ(detail::solveWeightedMasks(C, F.Input, Options, Budget, Report),
              F.Input);
  }
}

TEST(SymMBABitwiseView, DeepLiteralChainsRemainBounded) {
  SymContext C;
  auto X = C.mkVar("x", 257), View = X;
  for (unsigned Bit = 0; Bit != 96; ++Bit) {
    auto Mask = llvm::APInt::getOneBitSet(257, Bit);
    View = C.mkXor(C.mkAnd(View, C.mkConst(~Mask)), C.mkConst(Mask));
  }
  auto Input = C.mkAdd(C.mkXor(View, C.mkConst(257, 3)),
                       C.mkXor(View, C.mkConst(257, 5)));
  detail::WorkBudget Budget(65536);
  detail::SolveReport Report;
  EXPECT_EQ(detail::solveWeightedMasks(C, Input, {}, Budget, Report), Input);
  EXPECT_LT(Budget.used(), 65536u);
  detail::WorkBudget Proof(65536);
  EXPECT_FALSE(detail::proveMaskedSumIdentity(
      C, Input, C.mkMul(C.mkConst(257, 2), X), X, Proof));
  EXPECT_LT(Proof.used(), 65536u);
}

TEST(SymMBABitwiseView, IndependentZ3ChecksPartitionEquivalence) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 is unavailable";
  for (unsigned Width : {8u, 16u, 32u, 64u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width);
    auto F = flippedPartition(C, X, 1, 8);
    auto Result = simplifyMBADeep(C, F.Input);
    ASSERT_EQ(Result.Expr, F.Expected);
    solver::SymSynthVerifier Verify({}, solver::ProofBackend::Z3, 5000);
    EXPECT_EQ(Verify(C, F.Input, Result.Expr), SynthVerification::Equivalent);
  }
}

} // namespace
