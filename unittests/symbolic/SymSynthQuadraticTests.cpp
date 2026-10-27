//===- SymSynthQuadraticTests.cpp - Proved nonlinear oracle fitting ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verify quadratic recovery, modular ambiguity, proof refusal and budgets.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/solver/SymSynthVerifier.h"
#include "neverd/symbolic/SymExpr.h"
#include "neverd/symbolic/SymSynth.h"

#include <cstdint>
#include <string>

using namespace neverd::symbolic;
using namespace neverd::solver;

namespace {

SynthOptions quadraticOptions() {
  SynthOptions Opts = SynthOptions::sourceView();
  Opts.MaxConstants = 0;
  Opts.UseStochasticFallback = false;
  return Opts;
}

SymRef residual(SymContext &Ctx, SymRef X) {
  const unsigned W = Ctx.width(X);
  return Ctx.mkSub(
      Ctx.mkLShr(X, Ctx.mkConst(W, 4)),
      Ctx.mkLShr(Ctx.mkLShr(X, Ctx.mkConst(W, 2)), Ctx.mkConst(W, 2)));
}

SymRef polynomial(SymContext &Ctx, SymRef X, SymRef Y) {
  const unsigned W = Ctx.width(X);
  llvm::APInt Cross(W, 73);
  if (W > 64)
    Cross.setBit(W - 4);
  return Ctx.mkAdd({Ctx.mkMul(Ctx.mkConst(W, 11), Ctx.mkMul(X, X)),
                    Ctx.mkMul(Ctx.mkConst(Cross), Ctx.mkMul(X, Y)),
                    Ctx.mkMul(Ctx.mkConst(W, 182), Y), Ctx.mkConst(W, 37)});
}

SynthResult prove(SymContext &Ctx, SymRef E, const SynthOptions &Opts,
                  ProofBackend Backend = ProofBackend::BuiltIn) {
  SymSynthVerifier Verifier({}, Backend, 5000);
  return synthesize(Ctx, E, Opts, [&](SymContext &C, SymRef A, SymRef B) {
    return Verifier(C, A, B);
  });
}

TEST(SymSynthQuadratic, SourcePolicyEnablesTheSharedEnhancement) {
  EXPECT_TRUE(SynthOptions::sourceView().UseQuadraticFitting);
  EXPECT_FALSE(SynthOptions{}.UseQuadraticFitting);
}

TEST(SymSynthQuadratic, RecoversSquaresAndProductsAtEveryWordWidth) {
  for (unsigned W : {8u, 16u, 32u, 64u, 128u, 256u}) {
    SCOPED_TRACE(W);
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", W), Y = Ctx.mkVar("y", W);
    SymRef Expected = polynomial(Ctx, X, Y);
    SymRef E = Ctx.mkAdd(Expected, residual(Ctx, X));
    SynthResult R = prove(Ctx, E, quadraticOptions());
    ASSERT_TRUE(R.Changed) << synthOutcomeName(R.Outcome);
    EXPECT_EQ(R.Expr, Expected) << Ctx.toString(R.Expr);
    EXPECT_TRUE(R.FromQuadratic);
    EXPECT_FALSE(R.FromAffine);
    EXPECT_FALSE(R.FromSampler);
    EXPECT_EQ(R.Evidence, SynthEvidence::Verifier);
    EXPECT_EQ(R.ProofQueries, 1u);
    EXPECT_LT(R.SizeAfter, R.SizeBefore);
    EXPECT_LE(R.Work, quadraticOptions().MaxWork);
    RecordProperty("work_" + std::to_string(W), std::to_string(R.Work));
    RecordProperty("before_" + std::to_string(W), std::to_string(R.SizeBefore));
    RecordProperty("after_" + std::to_string(W), std::to_string(R.SizeAfter));
  }
}

TEST(SymSynthQuadratic, IndependentZ3ProvesCompleteWords) {
  if (!proofBackendAvailable(ProofBackend::Z3))
    GTEST_SKIP() << "Z3 backend unavailable";
  for (unsigned W : {8u, 16u, 32u, 64u, 128u, 256u}) {
    SCOPED_TRACE(W);
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", W), Y = Ctx.mkVar("y", W);
    SymRef Expected = polynomial(Ctx, X, Y);
    SynthResult R = prove(Ctx, Ctx.mkAdd(Expected, residual(Ctx, X)),
                          quadraticOptions(), ProofBackend::Z3);
    ASSERT_TRUE(R.Changed) << synthOutcomeName(R.Outcome);
    EXPECT_EQ(R.Expr, Expected);
    EXPECT_TRUE(R.FromQuadratic);
    EXPECT_EQ(R.Verification, SynthVerification::Equivalent);
  }
}

TEST(SymSynthQuadratic, SourceBudgetReusesExistingFactorsInEitherInputOrder) {
  for (unsigned W : {8u, 16u, 32u, 64u, 128u, 256u})
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(W);
      SCOPED_TRACE(Reverse);
      SymContext Ctx;
      SymRef First = Ctx.mkVar("first", W), Second = Ctx.mkVar("second", W);
      SymRef X = Reverse ? Second : First, Y = Reverse ? First : Second;
      SymRef Expected =
          Ctx.mkAdd({Ctx.mkMul(X, Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(W, 11), X),
                                            Ctx.mkMul(Ctx.mkConst(W, 73), Y))),
                     Ctx.mkMul(Ctx.mkConst(W, 182), Y), Ctx.mkConst(W, 37)});
      SymRef E = Ctx.mkAdd(Expected, residual(Ctx, X));
      SymSynthVerifier Verifier(SolverOptions::sourceView());
      SynthResult R = synthesize(
          Ctx, E, quadraticOptions(),
          [&](SymContext &C, SymRef A, SymRef B) { return Verifier(C, A, B); });
      ASSERT_TRUE(R.Changed) << synthOutcomeName(R.Outcome);
      EXPECT_EQ(R.Expr, Expected);
      EXPECT_TRUE(R.FromQuadratic);
      EXPECT_EQ(R.ProofQueries, 1u);
    }
}

TEST(SymSynthQuadratic, BooleanProductsDoNotNeedDivisionByTwo) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 1), Y = Ctx.mkVar("y", 1);
  SymRef E = Ctx.mkXor(Ctx.mkOr(X, Y), Ctx.mkXor(X, Y));
  SynthResult R = prove(Ctx, E, quadraticOptions());
  ASSERT_TRUE(R.Changed) << synthOutcomeName(R.Outcome);
  EXPECT_EQ(R.Expr, Ctx.mkMul(X, Y));
  EXPECT_TRUE(R.FromQuadratic);
}

TEST(SymSynthQuadratic, SharedNonlinearAddendsStillReturnARealCounterexample) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 256), Y = Ctx.mkVar("y", 256);
  SymRef Common = polynomial(Ctx, X, Y);
  SymRef Before = Ctx.mkAdd(Common, X), After = Ctx.mkAdd(Common, Y);
  SymSynthVerifier Verifier(SolverOptions::sourceView());
  ASSERT_EQ(Verifier(Ctx, Before, After), SynthVerification::Different);
  const auto &Model = Verifier.report().Counterexample;
  ASSERT_TRUE(Model.has_value());
  auto Values = Model->asVarValues(Ctx);
  EXPECT_NE(Ctx.eval(Before, Values), Ctx.eval(After, Values));
  EXPECT_EQ(Verifier.report().RejectedCandidate, After);
}

TEST(SymSynthQuadratic, SharedTermsDoNotCancelUnequalCoefficients) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 8), Y = Ctx.mkVar("y", 8);
  SymRef Product = Ctx.mkMul(X, Y);
  SymRef Before = Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(8, 3), Product), X);
  SymRef After = Ctx.mkAdd(Product, X);
  SymSynthVerifier Verifier(SolverOptions::sourceView());
  ASSERT_EQ(Verifier(Ctx, Before, After), SynthVerification::Different);
  ASSERT_TRUE(Verifier.report().Counterexample.has_value());
  auto Values = Verifier.report().Counterexample->asVarValues(Ctx);
  EXPECT_NE(Ctx.eval(Before, Values), Ctx.eval(After, Values));
}

TEST(SymSynthQuadratic, ExhaustiveBytePairsMatchIndependentArithmetic) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 8), Y = Ctx.mkVar("y", 8);
  SymRef E = Ctx.mkAdd(polynomial(Ctx, X, Y), residual(Ctx, X));
  SynthResult R = prove(Ctx, E, quadraticOptions());
  ASSERT_TRUE(R.Changed);
  SymEvalPlan Before(Ctx, E), After(Ctx, R.Expr);
  for (unsigned A = 0; A < 256; ++A)
    for (unsigned B = 0; B < 256; ++B) {
      llvm::APInt Values[] = {llvm::APInt(8, A), llvm::APInt(8, B)};
      llvm::APInt Want(8, 11 * A * A + 73 * A * B + 182 * B + 37, false, true);
      ASSERT_EQ(Before.eval(Values), Want);
      ASSERT_EQ(After.eval(Values), Want);
    }
}

TEST(SymSynthQuadratic, ResolvesTheNoninvertibleDiagonalWithoutLosingBits) {
  for (unsigned W : {8u, 16u, 32u, 64u, 128u, 256u}) {
    SCOPED_TRACE(W);
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", W);
    llvm::APInt A(W, 11);
    A.setBit(W - 1);
    SymRef E =
        Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(A), Ctx.mkMul(X, X)), residual(Ctx, X));
    // a and a+2^(w-1) are indistinguishable in the doubled-unit probe.
    // Choose the representative which eliminates the extra linear term,
    // retaining the complete coefficient rather than dropping its high bit.
    SymRef Expected = Ctx.mkMul(Ctx.mkConst(A), Ctx.mkMul(X, X));
    SynthResult R = prove(Ctx, E, quadraticOptions());
    ASSERT_TRUE(R.Changed) << synthOutcomeName(R.Outcome);
    EXPECT_EQ(R.Expr, Expected);
    EXPECT_EQ(R.Verification, SynthVerification::Equivalent);
  }
}

TEST(SymSynthQuadratic, FitsEightInputsWithinAnExplicitLargerBudget) {
  SymContext Ctx;
  llvm::SmallVector<SymRef, 8> Vars, Terms;
  for (unsigned I = 0; I < 8; ++I)
    Vars.push_back(Ctx.mkVar("v" + std::to_string(I), 32));
  for (unsigned I = 0; I < 7; ++I)
    Terms.push_back(
        Ctx.mkMul(Ctx.mkConst(32, 12 + I), Ctx.mkMul(Vars[I], Vars[I + 1])));
  SymRef Expected = Ctx.mkAdd(Terms);
  SynthOptions Opts = quadraticOptions();
  Opts.MaxCost = 64;
  Opts.MaxWork = 100000;
  SynthResult R = prove(Ctx, Ctx.mkAdd(Expected, residual(Ctx, Vars[0])), Opts);
  ASSERT_TRUE(R.Changed) << synthOutcomeName(R.Outcome);
  EXPECT_EQ(R.Expr, Expected);
  EXPECT_TRUE(R.FromQuadratic);
  EXPECT_EQ(R.NumLeaves, 8u);
  EXPECT_LE(R.Work, Opts.MaxWork);
}

TEST(SymSynthQuadratic, RestoresSignedAndUnsignedOpaqueViews) {
  SymContext Ctx;
  SymRef X = Ctx.mkZExt(Ctx.mkVar("byte", 8), 32);
  SymRef Y = Ctx.mkSExt(Ctx.mkVar("short", 16), 32);
  SymRef Expected = polynomial(Ctx, X, Y);
  SymRef E = Ctx.mkAdd(Expected, residual(Ctx, Ctx.mkMul(X, Y)));
  SynthResult R = prove(Ctx, E, quadraticOptions());
  ASSERT_TRUE(R.Changed) << synthOutcomeName(R.Outcome);
  EXPECT_EQ(R.Expr, Expected);
  SymSynthVerifier Independent;
  EXPECT_EQ(Independent(Ctx, E, R.Expr), SynthVerification::Equivalent);
}

TEST(SymSynthQuadratic, KeepsNarrowWrappingInsideARootConversion) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("byte", 8), Y = Ctx.mkVar("other", 8);
  SymRef Narrow = Ctx.mkAdd(polynomial(Ctx, X, Y), residual(Ctx, X));
  SymRef E = Ctx.mkZExt(Narrow, 32);
  SynthResult R = prove(Ctx, E, quadraticOptions());
  EXPECT_FALSE(R.Changed);
  EXPECT_EQ(R.Expr, E);
  EXPECT_EQ(R.ProofQueries, 0u);
}

TEST(SymSynthQuadratic, ImprovesTheSameSearchBudget) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 32), Y = Ctx.mkVar("y", 32);
  SymRef E = Ctx.mkAdd(polynomial(Ctx, X, Y), residual(Ctx, X));
  SynthOptions Opts = quadraticOptions();
  Opts.UseQuadraticFitting = false;
  SynthResult Baseline = prove(Ctx, E, Opts);
  EXPECT_FALSE(Baseline.Changed);
  EXPECT_EQ(Baseline.Outcome, SynthOutcome::BudgetExhausted);
  Opts.UseQuadraticFitting = true;
  SynthResult Enhanced = prove(Ctx, E, Opts);
  ASSERT_TRUE(Enhanced.Changed);
  EXPECT_LT(Enhanced.Work, Baseline.Work);
  RecordProperty("baseline_work", std::to_string(Baseline.Work));
  RecordProperty("enhanced_work", std::to_string(Enhanced.Work));
}

TEST(SymSynthQuadratic, UnknownStopsWithoutPublishingOrRepeatedProofs) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 32), Y = Ctx.mkVar("y", 32);
  SymRef E = Ctx.mkAdd(polynomial(Ctx, X, Y), residual(Ctx, X));
  unsigned Calls = 0;
  SynthResult R =
      synthesize(Ctx, E, quadraticOptions(), [&](SymContext &, SymRef, SymRef) {
        ++Calls;
        return SynthVerification::Unknown;
      });
  EXPECT_FALSE(R.Changed);
  EXPECT_EQ(R.Expr, E);
  EXPECT_EQ(R.Outcome, SynthOutcome::ProofIncomplete);
  EXPECT_EQ(R.ProofQueries, 1u);
  EXPECT_EQ(Calls, 1u);
}

TEST(SymSynthQuadratic, RejectsFullWidthSamplingCollisions) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 256), Y = Ctx.mkVar("y", 256);
  SymRef Rare = Ctx.mkAnd(Ctx.mkShl(Ctx.mkOne(256), Y),
                          Ctx.mkConst(llvm::APInt::getOneBitSet(256, 200)));
  SymRef E = Ctx.mkAdd({polynomial(Ctx, X, Y), residual(Ctx, X), Rare});
  SynthResult R = prove(Ctx, E, quadraticOptions());
  EXPECT_FALSE(R.Changed);
  EXPECT_EQ(R.Expr, E);
  EXPECT_GT(R.ProofQueries, 0u);
  EXPECT_NE(R.Verification, SynthVerification::Equivalent);
}

TEST(SymSynthQuadratic, RejectsCubicsBeforeProof) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 32);
  SymRef E =
      Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(32, 73), Ctx.mkMul(X, Ctx.mkMul(X, X))),
                residual(Ctx, X));
  unsigned Calls = 0;
  SynthResult R =
      synthesize(Ctx, E, quadraticOptions(), [&](SymContext &, SymRef, SymRef) {
        ++Calls;
        return SynthVerification::Unknown;
      });
  EXPECT_FALSE(R.Changed);
  EXPECT_EQ(R.Expr, E);
  EXPECT_EQ(Calls, 0u);
}

TEST(SymSynthQuadratic, SamplesAloneNeverAuthorizeAProductionRewrite) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 32), Y = Ctx.mkVar("y", 32);
  SymRef E = Ctx.mkAdd(polynomial(Ctx, X, Y), residual(Ctx, X));
  SynthResult R = discoverSynthesisCandidate(Ctx, E, quadraticOptions());
  EXPECT_TRUE(R.Changed);
  EXPECT_TRUE(R.FromQuadratic);
  EXPECT_EQ(R.Evidence, SynthEvidence::Samples);
  EXPECT_EQ(synthesizeEquivalent(Ctx, E, quadraticOptions()), E);
}

TEST(SymSynthQuadratic, CostAndWorkLimitsPreserveTheOriginal) {
  for (size_t Work : {size_t(0), size_t(1), size_t(100)}) {
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", 32), Y = Ctx.mkVar("y", 32);
    SymRef E = Ctx.mkAdd(polynomial(Ctx, X, Y), residual(Ctx, X));
    SynthOptions Opts = quadraticOptions();
    Opts.MaxWork = Work;
    SynthResult R = prove(Ctx, E, Opts);
    EXPECT_FALSE(R.Changed);
    EXPECT_EQ(R.Expr, E);
    EXPECT_EQ(R.ProofQueries, 0u);
    EXPECT_LE(R.Work, Work);
  }
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 32), Y = Ctx.mkVar("y", 32);
  SymRef E = Ctx.mkAdd(polynomial(Ctx, X, Y), residual(Ctx, X));
  SynthOptions Opts = quadraticOptions();
  Opts.MaxCost = 3;
  SynthResult R = prove(Ctx, E, Opts);
  EXPECT_FALSE(R.Changed);
  EXPECT_EQ(R.Expr, E);
  EXPECT_EQ(R.ProofQueries, 0u);
}

} // namespace
