//===- SymMBAMultiCarrierTests.cpp - Interleaved masked arithmetic --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check composition of distribution proofs and independent bitwise proofs.

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/solver/SymSynthVerifier.h"

#include <random>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

llvm::APInt randomCarrierWord(unsigned Width, std::mt19937_64 &Random) {
  llvm::APInt Value(Width, 0);
  for (unsigned Bit = 0; Bit < Width; Bit += 64)
    Value |= llvm::APInt(Width, Random()).shl(Bit);
  return Value;
}

struct InterleavedFixture {
  SymRef Input, Expected;
  std::vector<llvm::APInt> Keys;
};

InterleavedFixture interleaved(SymContext &C, llvm::ArrayRef<SymRef> Carriers,
                               int Weight, unsigned Count) {
  const unsigned Width = C.width(Carriers.front());
  std::mt19937_64 Random(0x362517 + Width);
  std::vector<llvm::SmallVector<SymRef, 4>> Columns(Count);
  std::vector<llvm::APInt> Keys;
  llvm::SmallVector<SymRef, 4> Expected;
  auto Scale = C.mkConst(llvm::APInt(Width, Weight, true));
  for (auto Carrier : Carriers) {
    std::vector<llvm::APInt> Masks(Count, llvm::APInt(Width, 0));
    for (unsigned Bit = 0; Bit != Width; ++Bit)
      Masks[Random() % Count].setBit(Bit);
    llvm::APInt Key(Width, 0);
    for (unsigned I = 0; I != Count; ++I) {
      auto Flip = randomCarrierWord(Width, Random);
      Key |= Flip & Masks[I];
      Columns[I].push_back(
          C.mkAnd(C.mkXor(Carrier, C.mkConst(Flip)), C.mkConst(Masks[I])));
    }
    Keys.push_back(Key);
    Expected.push_back(C.mkMul(Scale, C.mkXor(Carrier, C.mkConst(Key))));
  }
  llvm::SmallVector<SymRef, 16> Terms;
  for (const auto &Column : Columns)
    Terms.push_back(C.mkMul(Scale, C.mkAdd(Column)));
  return {C.mkAdd(Terms), C.mkAdd(Expected), std::move(Keys)};
}

TEST(SymMBAMultiCarrier, ReassemblesInterleavedPartitionsAcrossFullWidths) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    llvm::SmallVector<SymRef, 4> Carriers{
        C.mkAdd(X, Y), C.mkMul(X, Y), C.mkAnd(X, Y),
        C.mkXor(C.mkAdd(X, Y), C.mkMul(X, Y))};
    for (unsigned Count : {2u, 3u, 4u}) {
      for (int Weight : {37, -11, 182, 1}) {
        SCOPED_TRACE(Count);
        SCOPED_TRACE(Weight);
        auto F = interleaved(C, llvm::ArrayRef(Carriers).take_front(Count),
                             Weight, 16);
        detail::WorkBudget Budget(1 << 20);
        detail::SolveReport Report;
        auto Result =
            detail::solveWeightedMasks(C, F.Input, {}, Budget, Report);
        EXPECT_LT(C.readabilityCost(Result), C.readabilityCost(F.Input));
        EXPECT_LE(C.readabilityCost(Result), C.readabilityCost(F.Expected));
        EXPECT_EQ(Report.Evidence, MBAEvidence::Derivation);
        std::mt19937_64 Random(0x621913);
        for (unsigned Sample = 0; Sample != 64; ++Sample) {
          std::vector<llvm::APInt> Values{randomCarrierWord(Width, Random),
                                          randomCarrierWord(Width, Random)};
          EXPECT_EQ(C.eval(Result, Values), C.eval(F.Expected, Values));
        }
      }
    }
  }
}

TEST(SymMBAMultiCarrier, SharedSubtractionsPreserveScaledOpaqueRemainders) {
  for (unsigned Width : {8u, 32u, 64u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    for (int Weight : {7, -11, 182, 1}) {
      SCOPED_TRACE(Weight);
      std::mt19937_64 Random(0x681541);
      llvm::SmallVector<SymRef, 32> Positive, Negative, Expected;
      auto Scale = C.mkConst(llvm::APInt(Width, Weight, true));
      for (auto Carrier : {C.mkXor(X, Y), C.mkAdd(X, Y), C.mkMul(X, Y)}) {
        llvm::SmallVector<llvm::APInt, 4> Masks;
        llvm::APInt Union(Width, 0);
        for (unsigned I = 0; I != 4; ++I) {
          Masks.push_back(randomCarrierWord(Width, Random));
          Union |= Masks.back();
        }
        for (unsigned Subset = 1; Subset != 16; ++Subset) {
          auto Mask = llvm::APInt::getAllOnes(Width);
          for (unsigned I = 0; I != 4; ++I)
            if (Subset & (1u << I))
              Mask &= Masks[I];
          auto Term = C.mkAnd(Carrier, C.mkConst(Mask));
          (llvm::popcount(Subset) % 2 ? Positive : Negative).push_back(Term);
        }
        Expected.push_back(C.mkMul(Scale, C.mkAnd(Carrier, C.mkConst(Union))));
      }
      // Distribution must account for a flattened nonlinear product, repeated
      // residuals, a complement of arithmetic, and a literal constant.
      auto Residual = C.mkMul(C.mkConst(Width, 13), C.mkMul(X, X));
      Positive.push_back(Residual);
      Positive.push_back(Residual);
      Positive.push_back(C.mkNot(C.mkAdd(X, Y)));
      Negative.push_back(C.mkConst(Width, 31));
      auto Tail = C.mkOr(X, Y);
      auto Input = C.mkAdd(
          C.mkMul(Scale, C.mkSub(C.mkAdd(Positive), C.mkAdd(Negative))), Tail);
      Expected.push_back(
          C.mkMul(Scale, C.mkSub(C.mkAdd(C.mkMul(C.mkConst(Width, 2), Residual),
                                         C.mkNot(C.mkAdd(X, Y))),
                                 C.mkConst(Width, 31))));
      Expected.push_back(Tail);
      auto Want = C.mkAdd(Expected);
      detail::WorkBudget Budget(1 << 20);
      detail::SolveReport Report;
      auto Result = detail::solveWeightedMasks(C, Input, {}, Budget, Report);
      EXPECT_LT(C.readabilityCost(Result), C.readabilityCost(Input));
      EXPECT_EQ(Report.Evidence, MBAEvidence::Derivation);
      std::mt19937_64 Values(0x174635);
      for (unsigned Sample = 0; Sample != 128; ++Sample) {
        std::vector<llvm::APInt> Assignment{randomCarrierWord(Width, Values),
                                            randomCarrierWord(Width, Values)};
        EXPECT_EQ(C.eval(Result, Assignment), C.eval(Want, Assignment));
      }
    }
  }
}

TEST(SymMBAMultiCarrier, ArithmeticProofChecksBothDistributionsIndependently) {
  for (unsigned Width : {8u, 32u, 64u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto A = C.mkAnd(X, C.mkConst(Width, 0x79));
    auto B = C.mkXor(Y, C.mkConst(Width, 0x3d));
    auto Product = C.mkMul(X, Y);
    auto Scale = C.mkConst(llvm::APInt(Width, -11, true));
    auto Before = C.mkMul(Scale, C.mkAdd({A, B, Product, Product}));
    auto After =
        C.mkAdd({C.mkMul(Scale, A), C.mkMul(Scale, B),
                 C.mkMul(C.mkConst(llvm::APInt(Width, -22, true)), Product)});
    detail::WorkBudget Proof(1 << 20);
    EXPECT_TRUE(detail::proveArithmeticIdentity(C, Before, After, Proof));
    for (auto Wrong :
         {C.mkAdd(After,
                  C.mkConst(llvm::APInt::getOneBitSet(Width, Width - 1))),
          C.mkSub(After, C.mkMul(Scale, Product)),
          C.mkAdd(After, C.mkMul(X, C.mkAdd(X, C.mkOne(Width)))),
          C.mkMul(Scale, C.mkAdd({A, B, C.mkMul(X, X), Product}))}) {
      detail::WorkBudget WrongProof(1 << 20);
      EXPECT_FALSE(
          detail::proveArithmeticIdentity(C, Before, Wrong, WrongProof));
    }
    for (size_t Work : {0u, 1u, 16u}) {
      detail::WorkBudget Short(Work);
      EXPECT_FALSE(detail::proveArithmeticIdentity(C, Before, After, Short));
      EXPECT_LE(Short.used(), Work);
    }
    detail::WorkBudget NoStorage(1 << 20);
    EXPECT_FALSE(
        detail::proveArithmeticIdentity(C, Before, After, NoStorage, 0));
    detail::WorkBudget Invalid(1 << 20);
    EXPECT_FALSE(detail::proveArithmeticIdentity(C, {}, After, Invalid));
    EXPECT_FALSE(detail::proveArithmeticIdentity(C, Before, C.mkZero(Width + 1),
                                                 Invalid));
  }
}

TEST(SymMBAMultiCarrier, ExhaustiveBytePairsUseIndependentArithmeticOracle) {
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  auto F =
      interleaved(C, {C.mkXor(X, Y), C.mkMul(X, Y), C.mkAdd(X, Y)}, -11, 16);
  MBAOptions Options;
  Options.VerifySamples = 0;
  auto Result = simplifyMBADeep(C, F.Input, Options);
  ASSERT_LE(Result.SizeAfter, C.readabilityCost(F.Expected));
  for (unsigned A = 0; A != 256; ++A)
    for (unsigned B = 0; B != 256; ++B) {
      const unsigned Want =
          (-11u * (((A ^ B) ^ F.Keys[0].getZExtValue()) +
                   (((A * B) & 255) ^ F.Keys[1].getZExtValue()) +
                   (((A + B) & 255) ^ F.Keys[2].getZExtValue()))) &
          255;
      EXPECT_EQ(C.evalU64(Result.Expr, {A, B}), Want);
    }
}

TEST(SymMBAMultiCarrier, KeepsFactoredProofWhenChildRewritesHideCommonScales) {
  for (unsigned Width : {16u, 32u}) {
    for (bool IncludeAnd : {false, true}) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(IncludeAnd);
      SymContext C;
      auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
      llvm::SmallVector<SymRef, 4> Carriers{C.mkXor(X, Y), C.mkMul(X, Y)};
      llvm::SmallVector<llvm::APInt, 4> Unions{
          llvm::APInt(Width, IncludeAnd ? -133 : -1, true),
          llvm::APInt(Width, -129, true)};
      if (IncludeAnd) {
        Carriers.push_back(C.mkAnd(X, Y));
        Unions.emplace_back(Width, 0x7fd3);
      }
      std::mt19937_64 Random(0x194378);
      llvm::SmallVector<SymRef, 16> Positive, Negative, Expected;
      for (unsigned I = 0; I != Carriers.size(); ++I) {
        auto A = randomCarrierWord(Width, Random) & Unions[I];
        auto B = randomCarrierWord(Width, Random) & Unions[I];
        llvm::APInt Masks[] = {A, B, (Unions[I] & ~(A | B)) | (A & B)};
        for (unsigned Subset = 1; Subset != 8; ++Subset) {
          auto Mask = llvm::APInt::getAllOnes(Width);
          for (unsigned J = 0; J != 3; ++J)
            if (Subset & (1u << J))
              Mask &= Masks[J];
          (llvm::popcount(Subset) % 2 ? Positive : Negative)
              .push_back(C.mkAnd(Carriers[I], C.mkConst(Mask)));
        }
        Expected.push_back(C.mkAnd(Carriers[I], C.mkConst(Unions[I])));
      }
      auto Scale = C.mkConst(Width, 7), Tail = C.mkOr(X, Y);
      auto Input = C.mkAdd(
          C.mkMul(Scale, C.mkSub(C.mkAdd(Positive), C.mkAdd(Negative))), Tail);
      auto Want = C.mkAdd(C.mkMul(Scale, C.mkAdd(Expected)), Tail);
      MBAOptions Options;
      Options.MaxWork = 65536;
      Options.VerifySamples = 0;
      auto Result = simplifyMBADeep(C, Input, Options);
      EXPECT_LE(Result.SizeAfter, C.readabilityCost(Want))
          << C.toString(Result.Expr);
      for (unsigned I = 0; I != 128; ++I) {
        std::vector<llvm::APInt> Values{randomCarrierWord(Width, Random),
                                        randomCarrierWord(Width, Random)};
        EXPECT_EQ(C.eval(Result.Expr, Values), C.eval(Want, Values));
      }
    }
  }
}

TEST(SymMBAMultiCarrier, AlsoRetainsTheUnfactoredReadingOfRelatedWords) {
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  auto Sum = C.mkAdd(X, Y), Product = C.mkMul(X, Y), Tail = C.mkOr(X, Y);
  llvm::SmallVector<SymRef, 4> Terms;
  for (unsigned Mask : {0x39u, 0xc6u}) {
    llvm::SmallVector<SymRef, 4> Pieces;
    for (auto Carrier : {Sum, Product, C.mkXor(Sum, Product), Tail})
      Pieces.push_back(C.mkAnd(Carrier, C.mkConst(8, Mask)));
    Terms.push_back(C.mkMul(C.mkConst(8, 7), C.mkAdd(Pieces)));
  }
  Terms.push_back(Tail);
  auto Input = C.mkAdd(Terms);
  auto Want = C.mkAdd(C.mkMul(C.mkConst(8, 8), Tail),
                      C.mkMul(C.mkConst(8, 14), C.mkOr(Sum, Product)));
  MBAOptions Options;
  Options.MaxWork = 65536;
  Options.VerifySamples = 0;
  auto Result = simplifyMBADeep(C, Input, Options);
  EXPECT_LE(Result.SizeAfter, C.readabilityCost(Want))
      << C.toString(Result.Expr);
  for (unsigned A = 0; A != 256; ++A)
    for (unsigned B = 0; B != 256; ++B)
      EXPECT_EQ(C.evalU64(Result.Expr, {A, B}),
                (8 * (A | B) + 14 * (((A + B) & 255) | ((A * B) & 255))) & 255);
}

TEST(SymMBAMultiCarrier, NearbyHighBitErrorsRemainObservable) {
  for (unsigned Width : {8u, 32u, 64u, 257u}) {
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto F = interleaved(C, {C.mkXor(X, Y), C.mkMul(X, Y)}, 37, 8);
    auto Error =
        C.mkAnd(X, C.mkConst(llvm::APInt::getOneBitSet(Width, Width - 1)));
    auto Input = C.mkAdd(F.Input, Error);
    MBAOptions Options;
    Options.VerifySamples = 0;
    auto Result = simplifyMBADeep(C, Input, Options);
    ASSERT_LT(Result.SizeAfter, C.readabilityCost(Input));
    std::mt19937_64 Random(0x671223);
    for (unsigned Sample = 0; Sample != 128; ++Sample) {
      auto A = randomCarrierWord(Width, Random);
      A.setBit(Width - 1);
      std::vector<llvm::APInt> Values{A, randomCarrierWord(Width, Random)};
      auto Want = C.eval(F.Expected, Values) +
                  llvm::APInt::getOneBitSet(Width, Width - 1);
      EXPECT_EQ(C.eval(Result.Expr, Values), Want);
      EXPECT_NE(C.eval(Result.Expr, Values), C.eval(F.Expected, Values));
    }
  }
}

TEST(SymMBAMultiCarrier, SharedBudgetAndCacheDoNotAuthorizeIncompleteProofs) {
  SymContext C;
  auto X = C.mkVar("x", 64), Y = C.mkVar("y", 64);
  auto F = interleaved(C, {C.mkXor(X, Y), C.mkMul(X, Y)}, 37, 16);
  MBAOptions Options;
  Options.MaxWork = 32768;
  Options.VerifySamples = 0;
  MBARewriteCache Cache(C, 128);
  auto Result = simplifyMBADeep(C, F.Input, Options, Cache);
  EXPECT_LE(Result.SizeAfter, C.readabilityCost(F.Expected));
  EXPECT_LT(Result.Work, Options.MaxWork);
  EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
  EXPECT_EQ(simplifyMBADeep(C, F.Input, Options, Cache).Work, 1u);
  for (size_t Work : {0u, 1u, 64u, 256u, 1024u, 2048u}) {
    detail::WorkBudget Budget(Work);
    detail::SolveReport Report;
    EXPECT_EQ(detail::solveWeightedMasks(C, F.Input, {}, Budget, Report),
              F.Input);
    EXPECT_LE(Budget.used(), Work);
    EXPECT_NE(Report.Evidence, MBAEvidence::Derivation);
  }
  // A successful attempt is deterministic. One fewer unit cannot publish an
  // answer: the arithmetic check shares the budget of all per-word proofs.
  detail::WorkBudget Complete(1 << 20);
  detail::SolveReport CompleteReport;
  auto Answer =
      detail::solveWeightedMasks(C, F.Input, {}, Complete, CompleteReport);
  ASSERT_NE(Answer, F.Input);
  detail::WorkBudget OneShort(Complete.used() - 1);
  detail::SolveReport ShortReport;
  EXPECT_EQ(detail::solveWeightedMasks(C, F.Input, {}, OneShort, ShortReport),
            F.Input);
  for (size_t Bytes : {0u, 64u, 4096u, 16384u}) {
    Options.MaxTableBytes = Bytes;
    detail::WorkBudget Budget(1 << 20);
    detail::SolveReport Report;
    EXPECT_EQ(detail::solveWeightedMasks(C, F.Input, Options, Budget, Report),
              F.Input);
  }
}

TEST(SymMBAMultiCarrier, DenseWideCompositionsHaveCompleteCertificates) {
  for (unsigned Width : {32u, 64u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto F = interleaved(C, {X, Y}, 3, 4);
    detail::WorkBudget Budget(1 << 20);
    detail::SolveReport Report;
    auto Result = detail::solveWeightedMasks(C, F.Input, {}, Budget, Report);
    ASSERT_NE(Result, F.Input);
    ASSERT_EQ(Report.Evidence, MBAEvidence::Derivation);
    auto check = [&](const llvm::APInt &A, const llvm::APInt &B) {
      // Independent word arithmetic, including carries across both words.
      auto Want = ((A ^ F.Keys[0]) + (B ^ F.Keys[1])) * llvm::APInt(Width, 3);
      EXPECT_EQ(C.eval(Result, {A, B}), Want);
    };
    check(llvm::APInt(Width, 0), llvm::APInt(Width, 0));
    check(llvm::APInt::getAllOnes(Width), llvm::APInt::getAllOnes(Width));
    for (unsigned I = 0; I != 2 * Width; ++I)
      for (unsigned J = I; J != 2 * Width; ++J) {
        llvm::APInt A(Width, 0), B(Width, 0);
        (I < Width ? A : B).setBit(I % Width);
        (J < Width ? A : B).setBit(J % Width);
        check(A, B);
      }
  }
}

TEST(SymMBAMultiCarrier, IndependentZ3ChecksNarrowComposedProofs) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 is unavailable";
  // Dense multiword carry proofs at 32/64 bits exceed this independent
  // five-second gate. They retain complete derivation and full-word coverage
  // above; an SMT timeout is never counted as an equivalence proof.
  for (unsigned Width : {8u, 16u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto F = interleaved(C, {X, Y}, 3, 4);
    detail::WorkBudget Budget(1 << 20);
    detail::SolveReport Report;
    auto Result = detail::solveWeightedMasks(C, F.Input, {}, Budget, Report);
    ASSERT_NE(Result, F.Input);
    solver::SymSynthVerifier Verify({}, solver::ProofBackend::Z3, 5000);
    EXPECT_EQ(Verify(C, F.Input, Result), SynthVerification::Equivalent);
  }
}

} // namespace
