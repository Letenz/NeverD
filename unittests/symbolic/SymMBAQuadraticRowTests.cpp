//===- SymMBAQuadraticRowTests.cpp - Lazy polynomial coefficient rows -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent coefficient oracles for lazy rows and shared-power recovery.

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include <array>
#include <random>

using namespace neverd::symbolic;

namespace {

SymRef fromTable(SymContext &C, llvm::ArrayRef<SymRef> Inputs, uint16_t Bits) {
  llvm::SmallVector<SymRef, 16> Terms;
  for (unsigned M = 0; M != 16; ++M) {
    if (!(Bits & (1u << M)))
      continue;
    llvm::SmallVector<SymRef, 4> Factors;
    for (unsigned I = 0; I != 4; ++I)
      Factors.push_back(M & (1u << I) ? Inputs[I] : C.mkNot(Inputs[I]));
    Terms.push_back(C.mkAnd(Factors));
  }
  return Terms.empty() ? C.mkConst(C.width(Inputs[0]), 0) : C.mkOr(Terms);
}

void checkProduct(const detail::QuadraticProduct &Product, uint16_t A,
                  uint16_t B, const llvm::APInt &Scale) {
  for (unsigned I = 0; I != 16; ++I)
    for (unsigned J = I; J != 16; ++J) {
      unsigned Want = unsigned((A >> I & 1) && (B >> J & 1));
      unsigned Got =
          unsigned(Product.Factors[0].at(I) && Product.Factors[1].at(J));
      if (I != J) {
        Want += unsigned((A >> J & 1) && (B >> I & 1));
        Got += unsigned(Product.Factors[0].at(J) && Product.Factors[1].at(I));
      }
      EXPECT_EQ(Scale * llvm::APInt(Scale.getBitWidth(), Want, false, true),
                Product.Coefficient *
                    llvm::APInt(Scale.getBitWidth(), Got, false, true));
    }
}

TEST(SymMBAQuadraticRows, RecoversSplitFactorsWithWideAndNoninvertibleScales) {
  std::mt19937 Random(0x726f7773);
  for (unsigned Width : {1u, 3u, 8u, 64u, 257u})
    for (unsigned Sample = 0; Sample != 32; ++Sample) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Sample);
      SymContext C;
      llvm::SmallVector<SymRef, 4> Inputs;
      llvm::SmallVector<uint32_t, 4> Ids;
      for (const char *Name : {"x", "y", "z", "w"}) {
        auto V = C.mkVar(Name, Width);
        Inputs.push_back(V);
        C.collectVars(V, Ids);
      }
      const uint16_t A = Random(), B = Random();
      auto Scale = Sample % 2 ? llvm::APInt::getSignMask(Width)
                              : llvm::APInt(Width, 3, false, true);
      llvm::SmallVector<detail::PolyTerm, 8> Terms;
      for (uint16_t L : {uint16_t(0xaaaa), uint16_t(0x5555)})
        for (uint16_t R : {uint16_t(0xcccc), uint16_t(0x3333)})
          Terms.push_back(
              {Scale,
               {fromTable(C, Inputs, A & L), fromTable(C, Inputs, B & R)}});
      detail::WorkBudget Budget(65536);
      bool Determined = false;
      auto Product = detail::recoverQuadraticTerms(C, Terms, Ids, Width, Budget,
                                                   1 << 20, Determined);
      ASSERT_TRUE(Product);
      EXPECT_TRUE(Determined);
      EXPECT_LT(Budget.used(), 65536u);
      checkProduct(*Product, A, B, Scale);
    }
}

TEST(SymMBAQuadraticRows, CanceledMintermsDoNotBecomeFactorInputs) {
  SymContext C;
  std::array<SymRef, 4> Inputs{C.mkVar("x", 64), C.mkVar("y", 64),
                               C.mkVar("z", 64), C.mkVar("w", 64)};
  llvm::SmallVector<uint32_t, 4> Ids;
  for (auto V : Inputs)
    C.collectVars(V, Ids);
  auto A = fromTable(C, Inputs, 0x0033);
  auto B = fromTable(C, Inputs, 0xcc00);
  auto Extra = fromTable(C, Inputs, 0xffff);
  llvm::SmallVector<detail::PolyTerm, 8> Terms{
      {llvm::APInt(64, 6), {A, B}},
      {llvm::APInt(64, 17), {Extra, Extra}},
      {-llvm::APInt(64, 17), {Extra, Extra}}};
  detail::WorkBudget Budget(65536);
  bool Determined = false;
  auto Product = detail::recoverQuadraticTerms(C, Terms, Ids, 64, Budget,
                                               1 << 20, Determined);
  ASSERT_TRUE(Product);
  EXPECT_TRUE(Determined);
  checkProduct(*Product, 0x0033, 0xcc00, llvm::APInt(64, 6));
}

TEST(SymMBAQuadraticRows, ChecksBeyondAnchorRowsAndHonorsRefusal) {
  SymContext C;
  std::array<SymRef, 4> Inputs{C.mkVar("x", 257), C.mkVar("y", 257),
                               C.mkVar("z", 257), C.mkVar("w", 257)};
  llvm::SmallVector<uint32_t, 4> Ids;
  for (auto V : Inputs)
    C.collectVars(V, Ids);
  const auto Scale = llvm::APInt(257, 6);
  llvm::SmallVector<detail::PolyTerm, 8> Terms{
      {Scale, {fromTable(C, Inputs, 0x0033), fromTable(C, Inputs, 0xcc00)}}};
  detail::WorkBudget Full(65536);
  bool Determined = false;
  ASSERT_TRUE(detail::recoverQuadraticTerms(C, Terms, Ids, 257, Full, 1 << 20,
                                            Determined));
  for (size_t Work : {size_t(0), size_t(1), Full.used() - 1, Full.used()}) {
    detail::WorkBudget Budget(Work);
    auto Product = detail::recoverQuadraticTerms(C, Terms, Ids, 257, Budget,
                                                 1 << 20, Determined);
    EXPECT_EQ(bool(Product), Work == Full.used());
    EXPECT_EQ(Determined, bool(Product));
    EXPECT_LE(Budget.used(), Work);
  }
  for (size_t Bytes : {0u, 64u, 2048u}) {
    detail::WorkBudget Budget(65536);
    EXPECT_FALSE(detail::recoverQuadraticTerms(C, Terms, Ids, 257, Budget,
                                               Bytes, Determined));
    EXPECT_FALSE(Determined);
  }
  // The first nonzero edge is (0,10). Change only (4,14), leaving both
  // proposal rows intact; the complete matrix must reject this perturbation.
  Terms.push_back(
      {llvm::APInt(257, 1),
       {fromTable(C, Inputs, 1u << 4), fromTable(C, Inputs, 1u << 14)}});
  detail::WorkBudget Bad(65536);
  EXPECT_FALSE(detail::recoverQuadraticTerms(C, Terms, Ids, 257, Bad, 1 << 20,
                                             Determined));
  EXPECT_TRUE(Determined);
}

TEST(SymMBAQuadraticRows, CommonPowersLeaveOnlyTheLowDegreeProof) {
  for (unsigned Width : {3u, 8u, 64u, 257u}) {
    SymContext C;
    auto A = C.mkVar("a", Width), X = C.mkVar("x", Width);
    auto Y = C.mkVar("y", Width);
    auto Common = C.mkMul({A, A, A, A, A, A, A, A});
    auto Source = C.mkAdd(C.mkMul(Common, C.mkAnd(X, Y)),
                          C.mkMul(Common, C.mkAnd(X, C.mkNot(Y))));
    auto Expected = C.mkMul(Common, X);
    llvm::SmallVector<uint32_t, 4> Ids;
    C.collectVars(Source, Ids);
    llvm::SmallVector<SymRef, 4> Inputs;
    for (auto Id : Ids)
      Inputs.push_back(C.varRef(Id));
    MBAOptions Options;
    detail::WorkBudget Budget(4096);
    auto Result =
        detail::solvePolynomial(C, Source, Ids, Inputs, Options, Budget);
    ASSERT_TRUE(Result);
    EXPECT_EQ(*Result, Expected);
    detail::WorkBudget Proof(4096);
    EXPECT_TRUE(detail::provePolynomialIdentity(C, Source, Expected, 8, Proof,
                                                Options.MaxTableBytes));
    EXPECT_LT(Proof.used(), 4096u);
    detail::WorkBudget BadProof(4096);
    EXPECT_FALSE(detail::provePolynomialIdentity(
        C, Source, C.mkAdd(Expected, C.mkConst(Width, 1)), 8, BadProof,
        Options.MaxTableBytes));
  }
}

TEST(SymMBAQuadraticRows, RepeatedNormalizedMonomialsKeepModularCoefficients) {
  for (unsigned Width : {3u, 8u, 64u, 257u})
    for (unsigned Count : {2u, 7u, 16u, 33u}) {
      SymContext C;
      auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
      auto TwoX = C.mkMul(C.mkConst(Width, 2), X);
      auto Base =
          C.mkAdd(C.mkMul(C.mkAdd(TwoX, Y), C.mkSub(TwoX, Y)), C.mkMul(Y, Y));
      llvm::SmallVector<SymRef, 64> Factors(Count, Base);
      auto Source = C.mkMul(Factors);
      llvm::APInt Coefficient(Width, 1);
      for (unsigned I = 0; I != Count; ++I)
        Coefficient *= 4;
      Factors.assign(2 * Count, X);
      Factors.push_back(C.mkConst(Coefficient));
      auto Expected = C.mkMul(Factors);
      MBAOptions Options;
      Options.VerifySamples = 0;
      detail::WorkBudget Budget(65536);
      detail::SolveReport Report;
      EXPECT_EQ(detail::solveArithmetic(C, Source, Options, Budget, Report),
                Expected);
      EXPECT_EQ(Report.Evidence, MBAEvidence::Derivation);
    }
}

} // namespace
