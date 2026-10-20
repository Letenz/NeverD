//===- SymMBACubicTests.cpp - Direct cubic factor recovery ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent coefficient and word oracles for cubic product reconstruction.

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/solver/SymSynthVerifier.h"

#include <random>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

TruthTable table(unsigned Arity, uint64_t Bits) {
  auto T = TruthTable::zero(Arity);
  for (unsigned I = 0; I != T.entries(); ++I)
    T.setValue(I, (Bits >> I) & 1);
  return T;
}

// Multiply independently of anchor slices, membership counts and classes.
detail::HigherCoefficients expand(llvm::ArrayRef<TruthTable> Factors,
                                  const llvm::APInt &Scale) {
  detail::HigherCoefficients Out;
  for (unsigned I = 0; I != Factors[0].entries(); ++I)
    for (unsigned J = 0; J != Factors[1].entries(); ++J)
      for (unsigned K = 0; K != Factors[2].entries(); ++K) {
        if (!Factors[0].at(I) || !Factors[1].at(J) || !Factors[2].at(K))
          continue;
        detail::Monomial Key{I, J, K};
        llvm::sort(Key);
        auto [It, New] = Out.try_emplace(Key, Scale);
        if (!New)
          It->second += Scale;
      }
  std::erase_if(Out, [](const auto &Entry) { return Entry.second.isZero(); });
  return Out;
}

TEST(SymMBACubic, MatchesEveryTwoInputFactorTripleWithACommonMinterm) {
  for (unsigned A = 1; A != 16; ++A)
    for (unsigned B = A; B != 16; ++B)
      for (unsigned C = B; C != 16; ++C)
        for (unsigned Scale : {1u, 2u, 3u, 4u, 6u}) {
          llvm::SmallVector<TruthTable, 3> Factors{table(2, A), table(2, B),
                                                   table(2, C)};
          auto H = expand(Factors, llvm::APInt(3, Scale));
          detail::WorkBudget Budget(4096);
          auto R = detail::recoverCubicProduct(H, 2, 3, Budget, 65536);
          ASSERT_EQ(bool(R), bool(A & B & C) && Scale != 4)
              << A << " " << B << " " << C << " " << Scale;
          EXPECT_LT(Budget.used(), 4096u);
          if (R)
            EXPECT_EQ(expand(R->Factors, R->Coefficient), H);
        }
}

TEST(SymMBACubic, RecoversWideScalesAndChecksBeyondTheAnchorSlice) {
  std::mt19937_64 Random(0x63756265);
  for (unsigned Width : {2u, 8u, 16u, 32u, 64u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    for (unsigned Sample = 0; Sample != 128; ++Sample) {
      llvm::SmallVector<TruthTable, 3> Factors;
      for (unsigned I = 0; I != 3; ++I)
        Factors.push_back(table(3, Random() | 1));
      auto Scale = Sample % 2 ? llvm::APInt::getOneBitSet(Width, Width - 2)
                              : llvm::APInt(Width, Random() | 1, false, true);
      auto H = expand(Factors, Scale);
      detail::WorkBudget Budget(65536);
      auto R = detail::recoverCubicProduct(H, 3, Width, Budget, 65536);
      ASSERT_TRUE(R) << Sample;
      EXPECT_EQ(expand(R->Factors, R->Coefficient), H);
      // Minterm zero is the anchor. Altering a coefficient away from it
      // leaves every membership decision unchanged, but must fail validation.
      auto [It, New] =
          H.try_emplace(detail::Monomial{1, 2, 3}, llvm::APInt(Width, 0));
      It->second += 1;
      if (It->second.isZero())
        H.erase(It);
      detail::WorkBudget BadBudget(65536);
      EXPECT_FALSE(detail::recoverCubicProduct(H, 3, Width, BadBudget, 65536));
    }
  }
}

TEST(SymMBACubic, RefusesIncompleteWorkStorageAndMalformedCoefficients) {
  llvm::SmallVector<TruthTable, 3> Factors{table(3, 0xF9), table(3, 0xB7),
                                           table(3, 0xED)};
  auto H = expand(Factors, llvm::APInt(257, 7));
  detail::WorkBudget Full(65536);
  ASSERT_TRUE(detail::recoverCubicProduct(H, 3, 257, Full, 65536));
  for (size_t Work : {size_t(0), size_t(1), Full.used() - 1, Full.used()}) {
    detail::WorkBudget Budget(Work);
    EXPECT_EQ(bool(detail::recoverCubicProduct(H, 3, 257, Budget, 65536)),
              Work == Full.used());
    EXPECT_LE(Budget.used(), Work);
  }
  for (size_t Bytes : {0u, 64u, 512u, 1024u}) {
    detail::WorkBudget Budget(65536);
    EXPECT_FALSE(detail::recoverCubicProduct(H, 3, 257, Budget, Bytes));
  }
  auto Bad = [&](detail::Monomial Key, llvm::APInt Value) {
    auto Altered = H;
    Altered.insert_or_assign(std::move(Key), std::move(Value));
    detail::WorkBudget Budget(65536);
    EXPECT_FALSE(detail::recoverCubicProduct(Altered, 3, 257, Budget, 65536));
  };
  Bad({0, 1, 8}, llvm::APInt(257, 7));
  Bad({2, 1, 0}, llvm::APInt(257, 7));
  Bad({0, 1}, llvm::APInt(257, 7));
  Bad({0, 0, 0}, llvm::APInt(64, 7));
  Bad({0, 0, 0}, llvm::APInt(257, 0));
  Bad({0, 0, 0}, llvm::APInt::getSignMask(257));
}

struct Expansion {
  SymRef Before;
  SymRef Expected;
};

Expansion mixedExpansion(SymContext &C, unsigned Width, unsigned Family,
                         bool ExpandCube, bool NearMiss = false,
                         bool ExtraCube = true, unsigned Scale = 182) {
  auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
  auto Z = C.mkVar("z", Width), W = C.mkVar("w", Width);
  auto V = C.mkVar("v", Width);
  auto A = Family == 2 ? C.mkOr(X, Z) : C.mkXor(X, Y);
  auto B = Family == 0   ? C.mkOr(Y, Z)
           : Family == 1 ? C.mkOr(X, Y)
           : Family == 2 ? C.mkXor(Y, W)
                         : C.mkOr(Y, V);
  auto D = C.mkOr(X, Family == 3 ? V : Z);
  llvm::SmallVector<SymRef, 32> Terms;
  const unsigned Pieces = Family == 3 ? 4 : 2;
  for (unsigned I = 0; I != Pieces; ++I)
    for (unsigned J = 0; J != Pieces; ++J) {
      auto Left = C.mkAnd(A, I & 1 ? Z : C.mkNot(Z));
      auto Axis = Family == 1 ? W : X;
      auto Right = C.mkAnd(B, J & 1 ? Axis : C.mkNot(Axis));
      if (Pieces == 4) {
        Left = C.mkAnd(Left, I & 2 ? W : C.mkNot(W));
        Right = C.mkAnd(Right, J & 2 ? Z : C.mkNot(Z));
      }
      Terms.push_back(ExpandCube ? C.mkMul({Left, Right, D})
                                 : C.mkMul(Left, Right));
    }
  auto K = C.mkConst(Width, Scale);
  auto Cube = ExtraCube ? C.mkMul({C.mkConst(Width, 37), X, Y, Z})
                        : C.mkConst(Width, 0);
  auto Error = NearMiss ? C.mkAnd(X, C.mkConst(llvm::APInt::getSignMask(Width)))
                        : C.mkConst(Width, 0);
  auto Before =
      C.mkAdd({C.mkMul(K, C.mkAdd(Terms)), C.mkXor(X, Y),
               C.mkMul(C.mkConst(Width, 2), C.mkAnd(X, Y)), Cube, Error});
  auto Product = ExpandCube ? C.mkMul({K, A, B, D}) : C.mkMul({K, A, B});
  return {Before, C.mkAdd({Product, X, Y, Cube, Error})};
}

TEST(SymMBACubic, CompletesTheSixteenPreviouslyCappedMixedRegions) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 128u, 257u})
    for (unsigned Family = 0; Family != 4; ++Family) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Family);
      SymContext C;
      auto [Input, Expected] = mixedExpansion(C, Width, Family, false);
      MBAOptions Options;
      Options.MaxWork = 65536;
      Options.VerifySamples = 0;
      auto R = simplifyMBADeep(C, Input, Options);
      EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
      EXPECT_LT(R.Work, Options.MaxWork);
      EXPECT_LE(R.SizeAfter, C.readabilityCost(Expected));
      detail::WorkBudget Proof(65536);
      EXPECT_TRUE(detail::provePolynomialIdentity(C, Input, R.Expr, 5, Proof));
    }
}

TEST(SymMBACubic, RecoversCubesWithoutExpandingUnrelatedLinearInputs) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 128u, 257u})
    for (unsigned Family = 0; Family != 4; ++Family) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Family);
      SymContext C;
      auto [Input, Expected] =
          mixedExpansion(C, Width, Family, true, true, false, 37);
      MBAOptions Options;
      // Heap-backed coefficients pay for every limb comparison as well.
      Options.MaxWork = Width > 64 ? 131072 : 65536;
      Options.VerifySamples = 0;
      auto R = simplifyMBADeep(C, Input, Options);
      EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
      EXPECT_LT(R.Work, Options.MaxWork);
      EXPECT_LE(R.SizeAfter, C.readabilityCost(Expected));
      detail::WorkBudget Proof(65536);
      EXPECT_TRUE(detail::provePolynomialIdentity(C, Input, R.Expr, 6, Proof));
    }
}

TEST(SymMBACubic, WholeRootRequiresCompleteProofAndIndependentSMTAgreement) {
  for (unsigned Width : {2u, 3u, 32u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto [Input, Expected] = mixedExpansion(C, Width, 0, true, true, false, 7);
    detail::WorkBudget Full(65536);
    detail::SolveReport Complete;
    auto R = detail::solveExpandedProducts(C, Input, {}, Full, Complete);
    ASSERT_NE(R, Input);
    EXPECT_EQ(Complete.Evidence, MBAEvidence::Derivation);
    EXPECT_LE(C.readabilityCost(R), C.readabilityCost(Expected));
    for (size_t Work : {size_t(0), size_t(1), Full.used() - 1}) {
      detail::WorkBudget Budget(Work);
      detail::SolveReport Report;
      EXPECT_EQ(detail::solveExpandedProducts(C, Input, {}, Budget, Report),
                Input);
      EXPECT_NE(Report.Evidence, MBAEvidence::Derivation);
    }
    MBAOptions Tiny;
    Tiny.MaxTableBytes = 0;
    detail::WorkBudget Budget(65536);
    detail::SolveReport Report;
    EXPECT_EQ(detail::solveExpandedProducts(C, Input, Tiny, Budget, Report),
              Input);
    if (Width <= 3 && solver::proofBackendAvailable(solver::ProofBackend::Z3)) {
      solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 5000);
      EXPECT_EQ(Verifier(C, Input, R), SynthVerification::Equivalent);
    }
  }
}

TEST(SymMBACubic, PreservesExpandedCubesAndHighBitRemainders) {
  for (unsigned Width : {4u, 8u, 16u, 32u, 64u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    // Two different cubic products need not combine into a single product.
    // They must survive even when only part of the region can be recovered.
    auto [Input, Expected] = mixedExpansion(C, Width, 0, true, true);
    MBAOptions Options;
    Options.MaxWork = 65536;
    Options.VerifySamples = 0;
    auto R = simplifyMBADeep(C, Input, Options);
    std::mt19937_64 Random(0xabc123);
    for (unsigned Sample = 0; Sample != 256; ++Sample) {
      llvm::SmallVector<llvm::APInt, 5> Values;
      for (unsigned I = 0; I != 5; ++I) {
        llvm::APInt Value(Width, 0);
        for (unsigned Bit = 0; Bit < Width; Bit += 64)
          Value |= llvm::APInt(Width, Random(), false, true).shl(Bit);
        Values.push_back(Value);
      }
      auto X = Values[0], Y = Values[1], Z = Values[2];
      auto Oracle =
          llvm::APInt(Width, 182, false, true) * (X ^ Y) * (Y | Z) * (X | Z) +
          X + Y + llvm::APInt(Width, 37, false, true) * X * Y * Z +
          (X & llvm::APInt::getSignMask(Width));
      EXPECT_EQ(C.eval(Input, Values), Oracle);
      EXPECT_EQ(C.eval(R.Expr, Values), Oracle);
    }
    if (Width == 4) {
      SymEvalPlan Before(C, Input), After(C, R.Expr);
      for (unsigned Assignment = 0; Assignment != 4096; ++Assignment) {
        uint64_t X = Assignment & 15, Y = (Assignment >> 4) & 15,
                 Z = Assignment >> 8;
        llvm::SmallVector<uint64_t, 5> Values{X, Y, Z, 0, 0};
        auto Oracle = (182 * (X ^ Y) * (Y | Z) * (X | Z) + X + Y +
                       37 * X * Y * Z + (X & 8)) &
                      15;
        EXPECT_EQ(Before.evalU64(Values), Oracle);
        EXPECT_EQ(After.evalU64(Values), Oracle);
      }
    }
  }
}

} // namespace
