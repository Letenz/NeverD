//===- SymMBAConstantRegionTests.cpp - Constant abstraction boundaries ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "SymMBATestsDetail.h"

namespace {
using namespace neverd::symbolic;

SymRef complementaryInput(SymContext &Ctx, SymRef X, bool Or) {
  SymRef A = Ctx.mkNeg(X);
  SymRef B = Ctx.mkSub(X, Ctx.mkOne(Ctx.width(X)));
  return Or ? Ctx.mkOr(A, B) : Ctx.mkAnd(A, B);
}

TEST(SymMBAConstantRegion, PublishesAnExactConstantAbstraction) {
  for (unsigned Width : {1u, 3u, 64u, 257u}) {
    for (bool Or : {false, true}) {
      for (bool Deep : {false, true}) {
        SCOPED_TRACE(Width);
        SCOPED_TRACE(Or);
        SCOPED_TRACE(Deep);
        SymContext Ctx;
        SymRef X = Ctx.mkVar("x", Width);
        // Negation is the identity at width one. Keep both complete affine
        // sources visible there so this tests a constant abstraction boundary.
        SymRef Source = Width == 1 ? Ctx.mkAdd(X, Ctx.mkVar("y", Width)) : X;
        SymRef Input = complementaryInput(Ctx, Source, Or);
        auto Abstract = detail::abstractToMBA(Ctx, Input, false);
        ASSERT_TRUE(Abstract);
        EXPECT_TRUE(Ctx.isConst(Abstract->Body));
        MBAOptions Opts;
        Opts.VerifySamples = 0;
        auto Result = Deep ? simplifyMBADeep(Ctx, Input, Opts)
                           : simplifyMBA(Ctx, Input, Opts);
        if (Width != 1 || !Deep) {
          EXPECT_EQ(Result.Expr, Or ? Ctx.mkOnes(Width) : Ctx.mkZero(Width));
        } else {
          // Rewriting the children first can hide this relation again at one
          // bit. That is a separate opportunity; the value must remain exact.
          SymEvalPlan Plan(Ctx, Result.Expr);
          llvm::SmallVector<uint64_t, 4> Values(Ctx.numVars(), 0);
          for (unsigned K = 0; K < 4; ++K) {
            Values[0] = K & 1;
            Values[1] = K >> 1;
            EXPECT_EQ(Plan.evalU64(Values), Or ? 1u : 0u);
          }
        }
        EXPECT_LE(Result.SizeAfter, Result.SizeBefore);
        EXPECT_LE(Result.Work, Opts.MaxWork);
      }
    }
  }
}

TEST(SymMBAConstantRegion, RestoresOpaqueSourcesInFullValueConsumers) {
  for (unsigned Width : {1u, 3u, 64u, 257u}) {
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    SymRef Source = Ctx.mkAdd(Ctx.mkUDiv(X, Y), X);
    SymRef Zero = complementaryInput(Ctx, Source, false);
    SymRef Ones = complementaryInput(Ctx, Source, true);
    MBAOptions Opts;
    Opts.VerifySamples = 0;
    SymRef Sum = simplifyMBADeep(Ctx, Ctx.mkAdd(Y, Zero), Opts).Expr;
    SymRef Masked = simplifyMBADeep(Ctx, Ctx.mkAnd(X, Ones), Opts).Expr;
    if (Width != 1) {
      EXPECT_EQ(Sum, Y);
      EXPECT_EQ(Masked, X);
    } else {
      SymEvalPlan SumPlan(Ctx, Sum), MaskPlan(Ctx, Masked);
      llvm::SmallVector<uint64_t, 4> Values(Ctx.numVars(), 0);
      for (unsigned K = 0; K < 4; ++K) {
        Values[Ctx.varId(X)] = K & 1;
        Values[Ctx.varId(Y)] = K >> 1;
        EXPECT_EQ(SumPlan.evalU64(Values), Values[Ctx.varId(Y)]);
        EXPECT_EQ(MaskPlan.evalU64(Values), Values[Ctx.varId(X)]);
      }
    }
  }
}

TEST(SymMBAConstantRegion, AcceptsNoInputsWithoutEnablingSynthesis) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 64);
  MBAOptions Opts;
  Opts.MaxAtoms = 0;
  Opts.MaxSynthesisAtoms = 0;
  Opts.VerifySamples = 0;
  for (bool Or : {false, true}) {
    SymRef Input = complementaryInput(Ctx, X, Or);
    auto Result = simplifyMBA(Ctx, Input, Opts);
    EXPECT_EQ(Result.Expr, Or ? Ctx.mkOnes(64) : Ctx.mkZero(64));
    EXPECT_EQ(Result.NumAtoms, 0u);
    EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
  }
}

TEST(SymMBAConstantRegion, RequiresTheRemainingSingleCornerOfWork) {
  for (bool Or : {false, true}) {
    SymContext Ctx;
    SymRef Input = complementaryInput(Ctx, Ctx.mkVar("x", 64), Or);
    MBAOptions Opts;
    Opts.VerifySamples = 0;
    detail::WorkBudget Discovery(Opts.MaxWork);
    auto Abstract = detail::abstractToMBA(Ctx, Input, false, &Discovery);
    ASSERT_TRUE(Abstract);
    ASSERT_TRUE(Ctx.isConst(Abstract->Body));
    const size_t Work = Discovery.used();
    ASSERT_GT(Work, 0u);
    for (size_t Extra : {0u, 1u}) {
      detail::WorkBudget Budget(Work + Extra);
      detail::SolveReport Report;
      SymRef Result = detail::solveOneRegion(Ctx, Input, Opts, Budget, Report);
      EXPECT_EQ(Result, Extra ? Abstract->Body : Input);
      EXPECT_LE(Budget.used(), Work + Extra);
    }
  }
}

TEST(SymMBAConstantRegion, PreservesValidResultsUnderTinyResourceLimits) {
  for (unsigned Width : {1u, 3u, 64u, 257u}) {
    for (size_t Work : {0u, 1u, 16u, 64u, 256u, 1024u}) {
      for (size_t Bytes : {0u, 128u, 4096u}) {
        SymContext Ctx;
        SymRef Input = complementaryInput(Ctx, Ctx.mkVar("x", Width), false);
        MBAOptions Opts;
        Opts.VerifySamples = 0;
        Opts.MaxWork = Work;
        Opts.MaxTableBytes = Bytes;
        auto Result = simplifyMBA(Ctx, Input, Opts);
        EXPECT_LE(Result.Work, Work);
        EXPECT_TRUE(Result.Expr == Input || Ctx.isConstZero(Result.Expr));
        if (!Work)
          EXPECT_EQ(Result.Expr, Input);
      }
    }
  }
}

TEST(SymMBAConstantRegion, PreservesNearbyFalseComplementIdentities) {
  for (unsigned Width : {1u, 3u}) {
    for (unsigned Offset : {0u, 2u, 3u}) {
      for (bool Or : {false, true}) {
        SymContext Ctx;
        SymRef X = Ctx.mkVar("x", Width);
        SymRef A = Ctx.mkNeg(X);
        SymRef B = Ctx.mkSub(X, Ctx.mkConst(Width, Offset));
        SymRef Input = Or ? Ctx.mkOr(A, B) : Ctx.mkAnd(A, B);
        MBAOptions Opts;
        Opts.VerifySamples = 0;
        auto Result = simplifyMBADeep(Ctx, Input, Opts);
        SymEvalPlan Before(Ctx, Input), After(Ctx, Result.Expr);
        llvm::SmallVector<uint64_t, 1> Values(Ctx.numVars(), 0);
        for (unsigned Value = 0; Value < (1u << Width); ++Value) {
          Values[Ctx.varId(X)] = Value;
          EXPECT_EQ(Before.evalU64(Values), After.evalU64(Values));
        }
      }
    }
  }
}

} // namespace
