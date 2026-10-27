//===- SymSynthAffineTests.cpp - Proved semantic affine reconstruction ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verify modular affine synthesis, typed inputs, proof gating and budgets.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/solver/SymSynthVerifier.h"
#include "neverd/symbolic/SymExpr.h"
#include "neverd/symbolic/SymSynth.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace neverd::symbolic;
using namespace neverd::solver;

namespace {

SynthOptions affineOptions() {
  SynthOptions Opts;
  Opts.MaxCost = 48;
  Opts.MaxWork = 20000;
  Opts.MaxConstants = 0;
  Opts.MaxSamples = 64;
  Opts.VerifySamples = 64;
  Opts.UseStochasticFallback = false;
  return Opts;
}

// This residual is semantically zero, but neither identical nodes nor an
// identity within the MBA algebra: logical right shifts are its boundary.
SymRef shiftResidual(SymContext &Ctx, SymRef X) {
  const unsigned W = Ctx.width(X);
  return Ctx.mkSub(
      Ctx.mkLShr(X, Ctx.mkConst(W, 4)),
      Ctx.mkLShr(Ctx.mkLShr(X, Ctx.mkConst(W, 2)), Ctx.mkConst(W, 2)));
}

SymRef expression(SymContext &Ctx, unsigned Width, SymRef &Expected) {
  SymRef X = Ctx.mkVar("x", Width);
  SymRef Y = Ctx.mkVar("y", Width);
  SymRef Z = Ctx.mkVar("z", Width);
  llvm::APInt A(Width, 0xB6);
  // Exercise coefficients with significant bits outside uint64_t too.
  if (Width > 64)
    A.setBit(Width - 3);
  Expected = Ctx.mkAdd(
      {Ctx.mkMul(Ctx.mkConst(A), X), Ctx.mkMul(Ctx.mkConst(Width, 0x49), Y),
       Ctx.mkMul(Ctx.mkConst(Width, 0xD3), Z), Ctx.mkConst(Width, 0x25)});
  return Ctx.mkAdd(Expected, shiftResidual(Ctx, X));
}

SynthResult prove(SymContext &Ctx, SymRef E, const SynthOptions &Opts,
                  ProofBackend Backend = ProofBackend::BuiltIn) {
  SymSynthVerifier Verifier({}, Backend, 5000);
  return synthesize(Ctx, E, Opts, [&](SymContext &C, SymRef A, SymRef B) {
    return Verifier(C, A, B);
  });
}

TEST(SymSynthAffine, RecoversFullCoefficientVectorsAtEveryWordWidth) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 128u, 256u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    SymRef Expected;
    const SymRef E = expression(Ctx, Width, Expected);
    const SynthResult R = prove(Ctx, E, affineOptions());
    ASSERT_TRUE(R.Changed) << synthOutcomeName(R.Outcome);
    EXPECT_EQ(R.Expr, Expected) << Ctx.toString(R.Expr);
    EXPECT_TRUE(R.FromAffine);
    EXPECT_FALSE(R.FromSampler);
    EXPECT_EQ(R.Evidence, SynthEvidence::Verifier);
    EXPECT_EQ(R.ProofQueries, 1u);
    EXPECT_LT(R.SizeAfter, R.SizeBefore);
    EXPECT_LE(R.Work, affineOptions().MaxWork);
    RecordProperty("work_" + std::to_string(Width), std::to_string(R.Work));
    RecordProperty("before_" + std::to_string(Width),
                   std::to_string(R.SizeBefore));
    RecordProperty("after_" + std::to_string(Width),
                   std::to_string(R.SizeAfter));
  }
}

TEST(SymSynthAffine, AlsoRequiresFullWidthZ3Proof) {
  if (!proofBackendAvailable(ProofBackend::Z3))
    GTEST_SKIP() << "Z3 backend unavailable";
  for (unsigned Width : {8u, 16u, 32u, 64u, 128u, 256u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    SymRef Expected;
    const SymRef E = expression(Ctx, Width, Expected);
    const SynthResult R = prove(Ctx, E, affineOptions(), ProofBackend::Z3);
    ASSERT_TRUE(R.Changed) << synthOutcomeName(R.Outcome);
    EXPECT_EQ(R.Expr, Expected);
    EXPECT_EQ(R.Verification, SynthVerification::Equivalent);
    EXPECT_EQ(R.ProofQueries, 1u);
  }
}

TEST(SymSynthAffine, ExhaustsByteResultsAgainstIndependentArithmetic) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 8), Y = Ctx.mkVar("y", 8);
  const SymRef E = Ctx.mkAdd({Ctx.mkMul(Ctx.mkConst(8, 182), X),
                              Ctx.mkMul(Ctx.mkConst(8, 73), Y),
                              Ctx.mkConst(8, 37), shiftResidual(Ctx, X)});
  const SynthResult R = prove(Ctx, E, affineOptions());
  ASSERT_TRUE(R.Changed);
  SymEvalPlan Before(Ctx, E), After(Ctx, R.Expr);
  std::vector<llvm::APInt> Values;
  for (size_t I = 0; I < Ctx.numVars(); ++I)
    Values.emplace_back(Ctx.varInfo(I).Width, 0);
  for (unsigned A = 0; A < 256; ++A)
    for (unsigned B = 0; B < 256; ++B) {
      Values[Ctx.varId(X)] = llvm::APInt(8, A);
      Values[Ctx.varId(Y)] = llvm::APInt(8, B);
      const llvm::APInt Want(8, 182 * A + 73 * B + 37);
      ASSERT_EQ(Before.eval(Values), Want);
      ASSERT_EQ(After.eval(Values), Want);
    }
}

TEST(SymSynthAffine, FactorsEvenCoefficientsAcrossEightInputs) {
  SymContext Ctx;
  llvm::SmallVector<SymRef, 8> Vars, Terms;
  for (unsigned I = 0; I < 8; ++I) {
    Vars.push_back(Ctx.mkVar("x" + std::to_string(I), 32));
    Terms.push_back(Ctx.mkMul(Ctx.mkConst(32, 128), Vars.back()));
  }
  Terms.push_back(shiftResidual(Ctx, Vars.front()));
  SymRef E = Ctx.mkAdd(Terms);
  SymRef Expected = Ctx.mkMul(Ctx.mkConst(32, 128), Ctx.mkAdd(Vars));
  SynthOptions Opts = affineOptions();
  Opts.MaxWork = 30000;
  SynthResult R = prove(Ctx, E, Opts);
  ASSERT_TRUE(R.Changed) << synthOutcomeName(R.Outcome);
  EXPECT_EQ(R.Expr, Expected);
  EXPECT_TRUE(R.FromAffine);
  EXPECT_EQ(R.NumLeaves, 8u);
}

TEST(SymSynthAffine, PreservesNarrowInputsAndTheirTypedRelations) {
  SymContext Ctx;
  SymRef X = Ctx.mkZExt(Ctx.mkVar("byte", 8), 32);
  SymRef Y = Ctx.mkSExt(Ctx.mkVar("short", 16), 32);
  SymRef E =
      Ctx.mkAdd({Ctx.mkMul(Ctx.mkConst(32, 182), X),
                 Ctx.mkMul(Ctx.mkConst(32, 73), Y), shiftResidual(Ctx, X)});
  SymRef Expected = Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(32, 182), X),
                              Ctx.mkMul(Ctx.mkConst(32, 73), Y));
  SynthResult R = prove(Ctx, E, affineOptions());
  ASSERT_TRUE(R.Changed) << synthOutcomeName(R.Outcome);
  EXPECT_EQ(R.Expr, Expected);
  // Check the actual restored expression too, rather than trusting the
  // abstraction or treating sign extension as zero extension.
  SymSynthVerifier Independent;
  EXPECT_EQ(Independent(Ctx, E, R.Expr), SynthVerification::Equivalent);
}

TEST(SymSynthAffine, RecoversThroughAConversionAtTheRoot) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("byte", 8);
  SymRef E = Ctx.mkZExt(Ctx.mkAdd(X, shiftResidual(Ctx, X)), 32);
  SymRef Expected = Ctx.mkZExt(X, 32);
  SynthResult R = prove(Ctx, E, affineOptions());
  ASSERT_TRUE(R.Changed) << synthOutcomeName(R.Outcome);
  EXPECT_EQ(R.Expr, Expected);
  EXPECT_EQ(R.Evidence, SynthEvidence::Verifier);
  EXPECT_EQ(R.NumLeaves, 1u);
}

TEST(SymSynthAffine, TypedProblemConstructionRespectsResourceLimits) {
  for (size_t Work : {size_t(0), size_t(1), size_t(100)}) {
    SymContext Ctx;
    SymRef X = Ctx.mkVar("byte", 8);
    SymRef E = Ctx.mkZExt(Ctx.mkAdd(X, shiftResidual(Ctx, X)), 32);
    SynthOptions Opts = affineOptions();
    Opts.MaxWork = Work;
    SynthResult R = prove(Ctx, E, Opts);
    EXPECT_FALSE(R.Changed);
    EXPECT_EQ(R.Expr, E);
    EXPECT_EQ(R.Outcome, SynthOutcome::BudgetExhausted);
    EXPECT_LE(R.Work, Work);
    EXPECT_EQ(R.ProofQueries, 0u);
  }
  SymContext Ctx;
  SymRef X = Ctx.mkVar("byte", 8);
  SymRef E = Ctx.mkZExt(Ctx.mkAdd(X, shiftResidual(Ctx, X)), 32);
  SynthOptions Opts = affineOptions();
  Opts.MaxSamples = std::numeric_limits<size_t>::max();
  SynthResult R = prove(Ctx, E, Opts);
  EXPECT_EQ(R.Expr, E);
  EXPECT_EQ(R.Outcome, SynthOutcome::BudgetExhausted);
  EXPECT_LE(R.Work, Opts.MaxWork);
  EXPECT_EQ(R.ProofQueries, 0u);
  Opts.MaxSamples = affineOptions().MaxSamples;
  Opts.VerifySamples = std::numeric_limits<unsigned>::max();
  R = prove(Ctx, E, Opts);
  EXPECT_EQ(R.Expr, E);
  EXPECT_EQ(R.Outcome, SynthOutcome::BudgetExhausted);
  EXPECT_LE(R.Work, Opts.MaxWork);
  EXPECT_EQ(R.ProofQueries, 0u);
}

TEST(SymSynthAffine, TypedRootReportsItsActualInputLimit) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 8), Y = Ctx.mkVar("y", 8);
  SymRef E = Ctx.mkZExt(Ctx.mkAdd({X, Y, shiftResidual(Ctx, X)}), 32);
  SynthOptions Opts = affineOptions();
  Opts.MaxLeaves = 1;
  SynthResult R = prove(Ctx, E, Opts);
  EXPECT_FALSE(R.Changed);
  EXPECT_EQ(R.Expr, E);
  EXPECT_EQ(R.NumLeaves, 2u);
  EXPECT_EQ(R.Outcome, SynthOutcome::TooManyInputs);
  EXPECT_EQ(R.ProofQueries, 0u);
}

TEST(SymSynthAffine, DoesNotMoveNarrowOverflowAcrossAnExtension) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("byte", 8);
  // The inner product wraps at eight bits. It cannot become the affine
  // 32-bit product inferred from just the zero and one inputs.
  SymRef Narrow =
      Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(8, 182), X), shiftResidual(Ctx, X));
  SymRef E = Ctx.mkZExt(Narrow, 32);
  SynthResult R = prove(Ctx, E, affineOptions());
  EXPECT_FALSE(R.Changed);
  EXPECT_EQ(R.Expr, E);
  EXPECT_EQ(R.ProofQueries, 0u);
  SymEvalPlan Plan(Ctx, E);
  std::vector<llvm::APInt> Values;
  for (size_t I = 0; I < Ctx.numVars(); ++I)
    Values.emplace_back(Ctx.varInfo(I).Width, 0);
  for (unsigned A = 0; A < 256; ++A) {
    Values[Ctx.varId(X)] = llvm::APInt(8, A);
    EXPECT_EQ(Plan.eval(Values), llvm::APInt(32, (182 * A) & 255));
  }
}

TEST(SymSynthAffine, DefaultPolicyRecoversAnArbitraryCoefficient) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 64);
  SymRef Expected =
      Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(64, 12345), X), Ctx.mkConst(64, 37));
  SymRef E = Ctx.mkAdd(Expected, shiftResidual(Ctx, X));
  SynthResult R = prove(Ctx, E, {});
  ASSERT_TRUE(R.Changed) << synthOutcomeName(R.Outcome);
  EXPECT_EQ(R.Expr, Expected);
  EXPECT_TRUE(R.FromAffine);
  EXPECT_EQ(R.ProofQueries, 1u);
}

TEST(SymSynthAffine, ReadsTheNegativeUnitOfASignedBoolean) {
  SymContext Ctx;
  SymRef Flag = Ctx.mkVar("flag", 1), Byte = Ctx.mkVar("byte", 8);
  SymRef X = Ctx.mkSExt(Flag, 32), Y = Ctx.mkZExt(Byte, 32);
  SymRef Expected = Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(32, 17), X),
                              Ctx.mkMul(Ctx.mkConst(32, 182), Y));
  SymRef E = Ctx.mkAdd(Expected, shiftResidual(Ctx, Y));
  SynthResult R = prove(Ctx, E, affineOptions());
  ASSERT_TRUE(R.Changed) << synthOutcomeName(R.Outcome);
  EXPECT_EQ(R.Expr, Expected);
  EXPECT_EQ(R.NumLeaves, 2u);
  SymEvalPlan Plan(Ctx, R.Expr);
  std::vector<llvm::APInt> Values;
  for (size_t I = 0; I < Ctx.numVars(); ++I)
    Values.emplace_back(Ctx.varInfo(I).Width, 0);
  for (unsigned B = 0; B < 256; ++B)
    for (unsigned F = 0; F < 2; ++F) {
      Values[Ctx.varId(Flag)] = llvm::APInt(1, F);
      Values[Ctx.varId(Byte)] = llvm::APInt(8, B);
      EXPECT_EQ(Plan.eval(Values), llvm::APInt(32, 182u * B - 17u * F));
    }
}

TEST(SymSynthAffine, DoesNotConflateSignedAndUnsignedViewsOfOneInput) {
  SymContext Ctx;
  SymRef Byte = Ctx.mkVar("byte", 8);
  SymRef Signed = Ctx.mkSExt(Byte, 32), Unsigned = Ctx.mkZExt(Byte, 32);
  SymRef E = Ctx.mkAdd({Ctx.mkMul(Ctx.mkConst(32, 182), Signed),
                        Ctx.mkMul(Ctx.mkConst(32, 73), Unsigned),
                        shiftResidual(Ctx, Unsigned)});
  SynthOptions Opts = affineOptions();
  // Neither signed nor unsigned 255*x is equivalent. A tiny grammar also
  // cannot spell the piecewise correction at the sign boundary.
  Opts.MaxCost = 3;
  SynthResult R = prove(Ctx, E, Opts);
  EXPECT_FALSE(R.Changed);
  EXPECT_EQ(R.Expr, E);
}

TEST(SymSynthAffine, FindsAFormThatEnumerationCannotReachWithinTheSameBudget) {
  SymContext Ctx;
  SymRef Expected;
  SymRef E = expression(Ctx, 32, Expected);
  SynthOptions Opts = affineOptions();
  SynthResult Fit = prove(Ctx, E, Opts);
  ASSERT_TRUE(Fit.Changed);
  Opts.UseAffineFitting = false;
  SynthResult Enumerated = prove(Ctx, E, Opts);
  EXPECT_FALSE(Enumerated.Changed);
  EXPECT_EQ(Enumerated.Outcome, SynthOutcome::BudgetExhausted);
  EXPECT_LT(Fit.Work, Enumerated.Work);
  RecordProperty("affine_work", std::to_string(Fit.Work));
  RecordProperty("enumeration_work", std::to_string(Enumerated.Work));
}

TEST(SymSynthAffine, UnknownProofPreservesTheOriginalAndStopsSearch) {
  SymContext Ctx;
  SymRef Expected;
  SymRef E = expression(Ctx, 64, Expected);
  unsigned Calls = 0;
  SynthResult R =
      synthesize(Ctx, E, affineOptions(), [&](SymContext &, SymRef, SymRef) {
        ++Calls;
        return SynthVerification::Unknown;
      });
  EXPECT_EQ(R.Expr, E);
  EXPECT_FALSE(R.Changed);
  EXPECT_EQ(R.Outcome, SynthOutcome::ProofIncomplete);
  EXPECT_EQ(R.Evidence, SynthEvidence::None);
  EXPECT_EQ(Calls, 1u);
  EXPECT_EQ(R.ProofQueries, Calls);
}

TEST(SymSynthAffine, RejectsANonlinearFitThatAgreesAtBasisPoints) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 32), Y = Ctx.mkVar("y", 32);
  // x*x equals x at zero and one. The word-valued grid must reject the
  // affine fit, before the proof provider is ever consulted about it.
  SymRef E = Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(32, 182), Ctx.mkMul(X, X)),
                       Ctx.mkMul(Ctx.mkConst(32, 73), Y));
  unsigned Calls = 0;
  SynthResult R =
      synthesize(Ctx, E, affineOptions(), [&](SymContext &, SymRef, SymRef) {
        ++Calls;
        return SynthVerification::Unknown;
      });
  EXPECT_FALSE(R.Changed);
  EXPECT_EQ(R.Expr, E);
  EXPECT_EQ(Calls, 0u);
}

TEST(SymSynthAffine, NeverTreatsSamplesAsAProductionProof) {
  SymContext Ctx;
  SymRef Expected;
  SymRef E = expression(Ctx, 64, Expected);
  SynthResult R = discoverSynthesisCandidate(Ctx, E, affineOptions());
  ASSERT_TRUE(R.Changed);
  EXPECT_TRUE(R.FromAffine);
  EXPECT_EQ(R.Evidence, SynthEvidence::Samples);
  EXPECT_EQ(R.ProofQueries, 0u);
  EXPECT_EQ(synthesizeEquivalent(Ctx, E, affineOptions()), E);
}

TEST(SymSynthAffine, FullWidthProofRejectsASamplingCollision) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 256), Y = Ctx.mkVar("y", 256);
  SymRef Z = Ctx.mkVar("z", 256);
  SymRef Fit = Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(256, 7), X),
                         Ctx.mkMul(Ctx.mkConst(256, 11), Z));
  SymRef Rare = Ctx.mkAnd(Ctx.mkShl(Ctx.mkConst(256, 1), Y),
                          Ctx.mkConst(llvm::APInt::getOneBitSet(256, 200)));
  SymRef E = Ctx.mkAdd(Fit, Rare);
  SynthOptions Opts = affineOptions();
  Opts.MaxCost = 7;
  Opts.MaxWork = 40000;
  SymSynthVerifier Verifier;
  unsigned Rejected = 0;
  SynthResult R =
      synthesize(Ctx, E, Opts, [&](SymContext &C, SymRef A, SymRef B) {
        SynthVerification V = Verifier(C, A, B);
        if (B == Fit) {
          ++Rejected;
          EXPECT_EQ(V, SynthVerification::Different);
          const auto &Model = Verifier.report().Counterexample;
          EXPECT_TRUE(Model.has_value());
          if (Model) {
            auto Values = Model->asVarValues(C);
            EXPECT_NE(C.eval(A, Values), C.eval(B, Values));
          }
        }
        return V;
      });
  EXPECT_EQ(Rejected, 1u);
  EXPECT_FALSE(R.Changed);
  EXPECT_EQ(R.Expr, E);
  EXPECT_EQ(R.Evidence, SynthEvidence::None);
}

TEST(SymSynthAffine, RespectsCostAndWorkLimits) {
  for (size_t Work : {size_t(0), size_t(1), size_t(100)}) {
    SymContext Ctx;
    SymRef Expected;
    SymRef E = expression(Ctx, 64, Expected);
    SynthOptions Opts = affineOptions();
    Opts.MaxWork = Work;
    SynthResult R = prove(Ctx, E, Opts);
    EXPECT_EQ(R.Expr, E);
    EXPECT_FALSE(R.Changed);
    EXPECT_EQ(R.ProofQueries, 0u);
    EXPECT_LE(R.Work, Work);
  }
  SymContext Ctx;
  SymRef Expected;
  SymRef E = expression(Ctx, 32, Expected);
  SynthOptions Opts = affineOptions();
  Opts.MaxCost = 3;
  SynthResult R = prove(Ctx, E, Opts);
  EXPECT_FALSE(R.Changed);
  EXPECT_EQ(R.Expr, E);
  EXPECT_EQ(R.ProofQueries, 0u);
}

} // namespace
