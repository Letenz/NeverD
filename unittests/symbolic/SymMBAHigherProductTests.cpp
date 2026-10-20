//===- SymMBAHigherProductTests.cpp - Shared higher-degree cofactors ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent word oracles and refusal checks for higher MBA products.

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/solver/SymSynthVerifier.h"
#include "neverd/symbolic/SymParse.h"

#include <array>
#include <random>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

struct Expansion {
  SymRef Input;
  SymRef Expected;
};

Expansion partitioned(SymContext &C, unsigned Width, unsigned Degree,
                      unsigned Pieces, unsigned Scale = 37,
                      bool Perturbed = false) {
  auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
  auto Z = C.mkVar("z", Width), W = C.mkVar("w", Width);
  auto V = C.mkVar("v", Width);
  std::array<SymRef, 8> Words{
      C.mkXor(X, Y), C.mkOr(Y, Z),  C.mkOr(X, Z), C.mkAnd(X, Y),
      C.mkXor(X, Z), C.mkXor(Y, Z), C.mkOr(X, Y), C.mkNot(C.mkAnd(X, Z))};
  std::array<SymRef, 6> Axes{Z, X, Y, W, V, Z};
  llvm::SmallVector<SymRef, 16> Whole;
  for (unsigned I = 0; I != Degree; ++I)
    Whole.push_back(I < Words.size() ? Words[I] : X);
  llvm::SmallVector<SymRef, 64> Terms;
  SymRef Error = C.mkConst(Width, 0);
  for (unsigned Mask = 0; Mask != (1u << Pieces); ++Mask) {
    llvm::SmallVector<SymRef, 16> Factors;
    for (unsigned I = 0; I != Degree; ++I)
      Factors.push_back(
          I < Pieces
              ? C.mkAnd(Whole[I], Mask & (1u << I) ? Axes[I] : C.mkNot(Axes[I]))
              : Whole[I]);
    SymRef Term = C.mkMul(Factors);
    if (Perturbed && Mask == 0) {
      Error = C.mkMul(C.mkConst(Width, 2 * Scale), Term);
      Term = C.mkMul(C.mkConst(Width, 3), Term);
    }
    Terms.push_back(Term);
  }
  auto K = C.mkConst(Width, Scale);
  auto Residual =
      C.mkAdd(C.mkMul({C.mkConst(Width, 13), X, Y, Z}),
              C.mkAnd(X, C.mkConst(llvm::APInt::getSignMask(Width))));
  return {C.mkAdd({C.mkMul(K, C.mkAdd(Terms)), C.mkXor(X, Y),
                   C.mkMul(C.mkConst(Width, 2), C.mkAnd(X, Y)), Residual}),
          C.mkAdd({C.mkMul(K, C.mkMul(Whole)), X, Y, Residual, Error})};
}

llvm::APInt oracle(llvm::ArrayRef<llvm::APInt> V, unsigned Degree,
                   unsigned Scale) {
  auto X = V[0], Y = V[1], Z = V[2];
  unsigned Width = X.getBitWidth();
  std::array<llvm::APInt, 8> Words{X ^ Y, Y | Z, X | Z, X & Y,
                                   X ^ Z, Y ^ Z, X | Y, ~(X & Z)};
  llvm::APInt Product(Width, Scale, false, true);
  for (unsigned I = 0; I != Degree; ++I)
    Product *= I < Words.size() ? Words[I] : X;
  return Product + X + Y + llvm::APInt(Width, 13, false, true) * X * Y * Z +
         (X & llvm::APInt::getSignMask(Width));
}

TEST(SymMBAHigherProduct, RecoversPartitionsThroughTwelveFactorsAndWideWords) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 128u, 257u})
    for (unsigned Degree : {4u, 5u, 6u, 8u, 12u}) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Degree);
      SymContext C;
      auto [Input, Expected] = partitioned(C, Width, Degree, 4);
      MBAOptions Options;
      Options.MaxWork = 65536;
      Options.VerifySamples = 0;
      auto R = simplifyMBADeep(C, Input, Options);
      EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
      EXPECT_LT(R.Work, Options.MaxWork);
      EXPECT_LE(R.SizeAfter, C.readabilityCost(Expected)) << C.toString(R.Expr);
      std::mt19937_64 Random(0x48494748);
      for (unsigned Sample = 0; Sample != 128; ++Sample) {
        llvm::SmallVector<llvm::APInt, 5> Values;
        for (unsigned I = 0; I != 5; ++I) {
          llvm::APInt Value(Width, 0);
          for (unsigned Bit = 0; Bit < Width; Bit += 64)
            Value |= llvm::APInt(Width, Random(), false, true).shl(Bit);
          Values.push_back(Value);
        }
        auto Want = oracle(Values, Degree, 37);
        EXPECT_EQ(C.eval(Input, Values), Want);
        EXPECT_EQ(C.eval(R.Expr, Values), Want);
      }
    }
}

TEST(SymMBAHigherProduct, SharesProvedQuotientsAcrossSixtyFourProducts) {
  for (unsigned Scale : {1u, 37u, 182u, 0x80000000u}) {
    SymContext C;
    auto [Input, Expected] = partitioned(C, 32, 8, 6, Scale);
    MBAOptions Options;
    Options.MaxWork = 65536;
    Options.VerifySamples = 0;
    auto R = simplifyMBADeep(C, Input, Options);
    EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
    EXPECT_LT(R.Work, Options.MaxWork);
    EXPECT_LE(R.SizeAfter, C.readabilityCost(Expected));
  }
}

TEST(SymMBAHigherProduct, RecoversNonlinearQuotientsAndRepeatedFactors) {
  for (unsigned Width : {3u, 8u, 32u, 64u, 257u})
    for (const char *Text :
         {"(x&y)*(x|y)*(z&w)*(z|w)+(x&~y)*(~x&y)*(z&w)*(z|w)"
          "+(x&y)*(x|y)*(z&~w)*(~z&w)+(x&~y)*(~x&y)*(z&~w)*(~z&w)",
          "182*((x&y)*(x|y)*x*x*x*x+(x&~y)*(~x&y)*x*x*x*x)"}) {
      SymContext C;
      auto Input = parseSymExpr(C, Text, Width);
      ASSERT_TRUE(Input.ok());
      auto Expected = parseSymExpr(
          C, Text[0] == '1' ? "182*x*x*x*x*x*y" : "x*y*z*w", Width);
      ASSERT_TRUE(Expected.ok());
      MBAOptions Options;
      Options.MaxWork = 65536;
      Options.VerifySamples = 0;
      auto R = simplifyMBADeep(C, Input.Root, Options);
      EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
      EXPECT_LT(R.Work, Options.MaxWork);
      EXPECT_LE(R.SizeAfter, C.readabilityCost(Expected.Root));
      for (unsigned I = 0; I != 256; ++I) {
        std::array<llvm::APInt, 4> Values{
            llvm::APInt(Width, I, false, true),
            llvm::APInt(Width, I * 37, false, true),
            llvm::APInt(Width, I * 11, false, true),
            llvm::APInt(Width, I * 79, false, true)};
        EXPECT_EQ(C.eval(Input.Root, Values), C.eval(Expected.Root, Values));
        EXPECT_EQ(C.eval(R.Expr, Values), C.eval(Expected.Root, Values));
      }
    }
}

TEST(SymMBAHigherProduct,
     KeepsChangedCoefficientsAndPartialBudgetResultsExact) {
  for (bool Perturbed : {false, true}) {
    SymContext C;
    auto [Input, Expected] = partitioned(C, 3, 4, 4, 37, Perturbed);
    for (size_t Work : {0u, 1u, 128u, 1024u, 4096u, 65536u}) {
      detail::WorkBudget Budget(Work);
      detail::SolveReport Report;
      MBAOptions Options;
      Options.VerifySamples = 0;
      SymRef R =
          detail::solveProductCofactors(C, Input, Options, Budget, Report);
      EXPECT_LE(Budget.used(), Work);
      EXPECT_LE(C.readabilityCost(R), C.readabilityCost(Input));
      if (R != Input)
        EXPECT_EQ(Report.Evidence, MBAEvidence::Derivation);
      SymEvalPlan Before(C, Input), After(C, R), Want(C, Expected);
      for (unsigned Assignment = 0; Assignment != 4096; ++Assignment) {
        std::array<uint64_t, 5> Values{Assignment & 7u, (Assignment >> 3) & 7u,
                                       (Assignment >> 6) & 7u, Assignment >> 9,
                                       0};
        EXPECT_EQ(Before.evalU64(Values), Want.evalU64(Values));
        EXPECT_EQ(After.evalU64(Values), Want.evalU64(Values));
      }
    }
  }
}

TEST(SymMBAHigherProduct, RefusesBeforeAllocationAndRequiresDistributionProof) {
  SymContext C;
  auto [Input, Expected] = partitioned(C, 32, 4, 1);
  for (size_t Bytes : {0u, 128u}) {
    MBAOptions Options;
    Options.MaxTableBytes = Bytes;
    detail::WorkBudget Budget(65536);
    detail::SolveReport Report;
    size_t Nodes = C.numNodes();
    EXPECT_EQ(detail::solveProductCofactors(C, Input, Options, Budget, Report),
              Input);
    EXPECT_EQ(C.numNodes(), Nodes);
  }
  detail::WorkBudget Full(65536);
  detail::SolveReport Report;
  auto R = detail::solveProductCofactors(C, Input, {}, Full, Report);
  ASSERT_NE(R, Input);
  for (size_t Work : {size_t(0), size_t(1), Full.used() - 1, Full.used()}) {
    detail::WorkBudget Budget(Work);
    detail::SolveReport Limited;
    auto Value = detail::solveProductCofactors(C, Input, {}, Budget, Limited);
    EXPECT_EQ(Value == Input, Work != Full.used());
    EXPECT_LE(Budget.used(), Work);
  }
}

TEST(SymMBAHigherProduct, IndependentSmallWordZ3Proofs) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {2u, 3u})
    for (unsigned Degree : {4u, 6u}) {
      SymContext C;
      auto [Input, Expected] = partitioned(C, Width, Degree, 3);
      MBAOptions Options;
      Options.MaxWork = 65536;
      auto R = simplifyMBADeep(C, Input, Options);
      ASSERT_NE(R.Expr, Input);
      solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 5000);
      EXPECT_EQ(Verifier(C, Input, R.Expr), SynthVerification::Equivalent);
    }
}

TEST(SymMBAHigherProduct, RefusesFlatteningAnOversizedOpaqueQuotient) {
  SymContext C;
  llvm::SmallVector<SymRef, 512> Wide;
  for (unsigned I = 0; I != 512; ++I)
    Wide.push_back(C.mkVar("v" + std::to_string(I), 32));
  auto A = C.mkVar("a", 32), B = C.mkVar("b", 32);
  auto D = C.mkVar("d", 32), X = C.mkVar("x", 32);
  auto Input =
      C.mkAdd(C.mkMul({A, B, D, C.mkAdd(Wide)}), C.mkMul({A, B, D, X}));
  size_t Nodes = C.numNodes();
  detail::WorkBudget Budget(65536);
  detail::SolveReport Report;
  EXPECT_EQ(detail::solveProductCofactors(C, Input, {}, Budget, Report), Input);
  EXPECT_TRUE(Report.BudgetExhausted);
  EXPECT_LE(C.numNodes(), Nodes + 4);
  EXPECT_LT(Budget.used(), 512u);
}

TEST(SymMBAHigherProduct, CoefficientBoundsAdmitIndependentModularExpansions) {
  std::mt19937_64 Random(0x424f554e44);
  for (unsigned Width : {2u, 8u, 32u, 64u, 257u})
    for (unsigned Degree : {4u, 5u, 6u, 8u})
      for (unsigned Sample = 0; Sample != 16; ++Sample) {
        llvm::APInt Scale = Sample % 3 == 0
                                ? llvm::APInt::getOneBitSet(Width, Width - 2)
                                : llvm::APInt(Width, Random() | 1, false, true);
        if (Sample % 3 == 2)
          Scale <<= 1;
        std::vector<unsigned> Other(Degree);
        for (auto &Value : Other)
          Value = 1 + Random() % 3;
        detail::HigherCoefficients H;
        // Enumerate choices independently of coefficient bounds or inverses.
        for (unsigned Mask = 0; Mask != (1u << Degree); ++Mask) {
          detail::Monomial Key;
          for (unsigned I = 0; I != Degree; ++I)
            Key.push_back(Mask & (1u << I) ? Other[I] : 0);
          llvm::sort(Key);
          auto [It, New] = H.try_emplace(Key, Scale);
          if (!New)
            It->second += Scale;
        }
        std::erase_if(H,
                      [](const auto &Entry) { return Entry.second.isZero(); });
        detail::WorkBudget Full(65536);
        ASSERT_TRUE(
            detail::productCoefficientsFitScale(H, Degree, Scale, Full));
        detail::WorkBudget Short(Full.used() - 1);
        EXPECT_FALSE(
            detail::productCoefficientsFitScale(H, Degree, Scale, Short));
      }
  for (unsigned Scale : {1u, 2u}) {
    detail::HigherCoefficients Bad;
    Bad.try_emplace(detail::Monomial{0, 0, 0, 1}, llvm::APInt(32, 5));
    detail::WorkBudget Budget(65536);
    EXPECT_FALSE(detail::productCoefficientsFitScale(
        Bad, 4, llvm::APInt(32, Scale), Budget));
  }
}

TEST(SymMBAHigherProduct, CancelsExactResidualsBeforePolynomialProofExpansion) {
  for (unsigned Width : {8u, 32u, 64u, 257u}) {
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto Z = C.mkVar("z", Width);
    llvm::SmallVector<SymRef, 16> Factors;
    for (unsigned I = 0; I != 16; ++I)
      Factors.push_back(I % 2 ? C.mkOr(Y, Z) : C.mkXor(X, Y));
    auto Residual = C.mkMul(Factors);
    auto Scale = C.mkConst(Width, 37);
    auto Input =
        C.mkMul(Scale, C.mkAdd({Residual, C.mkXor(X, Y),
                                C.mkMul(C.mkConst(Width, 2), C.mkAnd(X, Y))}));
    auto Expected =
        C.mkAdd(C.mkMul(Scale, Residual), C.mkMul(Scale, C.mkAdd(X, Y)));
    detail::WorkBudget Full(4096);
    ASSERT_TRUE(detail::provePolynomialIdentity(C, Input, Expected, 3, Full));
    detail::WorkBudget Wrong(4096);
    EXPECT_FALSE(detail::provePolynomialIdentity(
        C, Input, C.mkAdd(Expected, C.mkOne(Width)), 3, Wrong));
    detail::WorkBudget NoStorage(4096);
    EXPECT_FALSE(
        detail::provePolynomialIdentity(C, Input, Expected, 3, NoStorage, 0));
  }
}

} // namespace
