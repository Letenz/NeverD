//===- SymMBAMaskRelationTests.cpp - Related literal mask inputs
//-----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks exact full-word mask relations without assuming partial complements.

#include "gtest/gtest.h"

#include "neverd/solver/SymSynthVerifier.h"
#include "neverd/symbolic/SymMBA.h"

#include <random>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

std::pair<SymRef, SymRef> maskedComplement(SymContext &C, unsigned Width,
                                           bool NearMiss = false,
                                           bool SecondMask = false) {
  auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
  if (SecondMask)
    Y = C.mkXor(Y, C.mkConst(llvm::APInt(Width, 0x6d)));
  llvm::APInt Mask(Width, 0x39);
  auto A = C.mkXor(X, C.mkConst(Mask));
  auto B = C.mkXor(X, C.mkConst(~Mask ^ llvm::APInt(Width, NearMiss)));
  auto K = C.mkConst(llvm::APInt(Width, 37));
  auto Tail = C.mkConst(llvm::APInt(Width, 0x55));
  auto Input = C.mkOr(C.mkAdd(C.mkMul(K, C.mkAdd(C.mkOr(A, Y), B)), K), Tail);
  auto Expected = C.mkOr(C.mkMul(K, C.mkAnd(C.mkNot(A), Y)), Tail);
  return {Input, Expected};
}

TEST(SymMBAMaskRelation, RecoversComplementedMasksAtTheCompleteWidth) {
  for (bool SecondMask : {false, true})
    for (unsigned Width : {8u, 16u, 32u, 64u, 128u, 257u}) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(SecondMask);
      SymContext C;
      auto [Input, Expected] = maskedComplement(C, Width, false, SecondMask);
      MBAOptions Options;
      Options.MaxWork = 1 << 16;
      Options.VerifySamples = 0;
      auto Result = simplifyMBADeep(C, Input, Options);
      EXPECT_TRUE(Result.Changed) << C.toString(Result.Expr);
      EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
      EXPECT_LE(C.readabilityCost(Result.Expr), C.readabilityCost(Expected));
      solver::SolverOptions ProofOptions;
      // The production verifier defaults to a 256-bit blasting limit. This
      // test deliberately exercises a wider word with the same finite SAT
      // and gate budgets; an unsupported-width refusal is not a proof failure.
      ProofOptions.Blast.MaxWidth = Width;
      solver::SymSynthVerifier Verify(ProofOptions);
      EXPECT_EQ(Verify(C, Expected, Result.Expr),
                SynthVerification::Equivalent);
      if (Width <= 32)
        EXPECT_EQ(Verify(C, Input, Result.Expr), SynthVerification::Equivalent);

      // Exercise every word, including the partial final word at width 257,
      // against independent APInt arithmetic rather than a truncated u64
      // oracle.
      std::mt19937_64 Random(0x3917);
      for (unsigned Sample = 0; Sample != 128; ++Sample) {
        llvm::SmallVector<llvm::APInt, 8> Values(C.numVars(),
                                                 llvm::APInt(Width, 0));
        for (unsigned V = 0; V != 2; ++V)
          for (unsigned Bit = 0; Bit < Width; Bit += 64)
            Values[V] |= llvm::APInt(Width, Random()).shl(Bit);
        const auto X = Values[0] ^ llvm::APInt(Width, 0x39);
        const auto Y = Values[1] ^ llvm::APInt(Width, SecondMask ? 0x6d : 0);
        const auto Oracle =
            (llvm::APInt(Width, 37) * ((X | Y) + ~X) + llvm::APInt(Width, 37)) |
            llvm::APInt(Width, 0x55);
        EXPECT_EQ(C.eval(Result.Expr, Values), Oracle);
      }
    }
}

TEST(SymMBAMaskRelation, ExhaustiveBytePairsIncludeNearbyNonidentities) {
  for (bool SecondMask : {false, true})
    for (bool NearMiss : {false, true}) {
      SymContext C;
      auto [Input, Expected] = maskedComplement(C, 8, NearMiss, SecondMask);
      auto Result = simplifyMBADeep(C, Input);
      bool DiffersFromComplement = false;
      for (unsigned X = 0; X != 256; ++X)
        for (unsigned Y = 0; Y != 256; ++Y) {
          llvm::SmallVector<uint64_t, 8> Values(C.numVars(), 0);
          Values[0] = X;
          Values[1] = Y;
          const auto Oracle =
              uint8_t((37u * ((X ^ 0x39u) | (Y ^ (SecondMask ? 0x6du : 0u))) +
                       37u * (X ^ (0xc6u ^ unsigned(NearMiss))) + 37u) |
                      0x55u);
          ASSERT_EQ(C.evalU64(Result.Expr, Values), Oracle);
          DiffersFromComplement |= C.evalU64(Expected, Values) != Oracle;
        }
      EXPECT_EQ(DiffersFromComplement, NearMiss);
    }
}

TEST(SymMBAMaskRelation, PreservesSemanticsWithIncompleteRelationBudgets) {
  for (bool SecondMask : {false, true})
    for (size_t Limit : {size_t(0), size_t(1), size_t(32), size_t(256)}) {
      SymContext C;
      auto [Input, Expected] = maskedComplement(C, 32, false, SecondMask);
      MBAOptions Options;
      Options.MaxWork = Limit;
      auto Result = simplifyMBADeep(C, Input, Options);
      EXPECT_LE(Result.Work, Limit);
      solver::SymSynthVerifier Verify;
      EXPECT_EQ(Verify(C, Input, Result.Expr), SynthVerification::Equivalent);
    }
}

TEST(SymMBAMaskRelation, IndependentZ3ChecksMaskedArithmeticBeforeScaling) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 is unavailable";
  for (unsigned Width : {8u, 16u, 32u, 64u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    const llvm::APInt Mask(Width, 0x39);
    auto A = C.mkXor(X, C.mkConst(Mask));
    auto B = C.mkXor(Y, C.mkConst(llvm::APInt(Width, 0x6d)));
    auto Input =
        C.mkAdd({C.mkOr(A, B), C.mkXor(X, C.mkConst(~Mask)), C.mkOne(Width)});
    auto Result = simplifyMBADeep(C, Input);
    ASSERT_TRUE(Result.Changed);
    // Prove the complete-width identity before the outer scaling and mask.
    // Congruence preserves it under those operations; the separate wrapped
    // tests exercise composition, high bits and nearby nonidentities.
    solver::SymSynthVerifier Verify({}, solver::ProofBackend::Z3, 5000);
    EXPECT_EQ(Verify(C, Input, Result.Expr), SynthVerification::Equivalent);
  }
}

TEST(SymMBAMaskRelation, SmallStorageAllowancesNeverCreateFalseRelations) {
  for (size_t Bytes : {size_t(0), size_t(64), size_t(256)}) {
    SymContext C;
    auto [Input, Expected] = maskedComplement(C, 8, true, true);
    MBAOptions Options;
    Options.MaxTableBytes = Bytes;
    auto Result = simplifyMBADeep(C, Input, Options);
    solver::SymSynthVerifier Verify;
    EXPECT_EQ(Verify(C, Input, Result.Expr), SynthVerification::Equivalent);
  }
}

} // namespace
