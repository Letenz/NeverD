//===- SymMBASampleTests.cpp - Deterministic sample checks ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include <random>
#include <utility>
#include <vector>

using namespace neverd::symbolic;

namespace {

using Assignment = std::vector<llvm::APInt>;

std::vector<Assignment> referenceAssignments(const SymContext &Ctx,
                                             unsigned Samples) {
  std::mt19937_64 Rng(0x9E3779B97F4A7C15ull);
  std::vector<Assignment> Out;
  for (unsigned S = 0; S < Samples; ++S) {
    Assignment Values;
    for (size_t I = 0; I < Ctx.numVars(); ++I) {
      const unsigned W = Ctx.varInfo(uint32_t(I)).Width;
      if (S == 0)
        Values.emplace_back(W, 0);
      else if (S == 1)
        Values.push_back(llvm::APInt::getAllOnes(W));
      else if (S == 2)
        Values.emplace_back(W, 1);
      else {
        std::vector<uint64_t> Words((W + 63) / 64);
        for (uint64_t &Word : Words)
          Word = Rng();
        Values.emplace_back(W, Words);
      }
    }
    Out.push_back(std::move(Values));
  }
  return Out;
}

bool referenceAgree(const SymContext &Ctx, SymRef A, SymRef B,
                    unsigned Samples) {
  SymEvalPlan PlanA(Ctx, A), PlanB(Ctx, B);
  // The reference retains APInt throughout. Plan::eval delegates each
  // operation to evalNodeAP, so no operator semantics are duplicated here.
  for (const Assignment &Values : referenceAssignments(Ctx, Samples))
    if (PlanA.eval(Values) != PlanB.eval(Values))
      return false;
  return true;
}

// Isolate a particular assignment without requiring the tested operation to
// have different values at every corner. The deliberately wrong constant
// ensures that evaluating neither branch cannot accidentally pass the test.
void expectAtSample(SymContext &Ctx, SymRef E, SymRef Selector,
                    unsigned Sample) {
  const auto Values = referenceAssignments(Ctx, Sample + 1);
  SymEvalPlan Plan(Ctx, E);
  llvm::APInt Expected = Plan.eval(Values.back());
  SymRef Gate =
      Ctx.mkEq(Selector, Ctx.mkConst(Values.back()[Ctx.varId(Selector)]));
  SymRef Zero = Ctx.mkZero(Ctx.width(E));
  SymRef Actual = Ctx.mkIte(Gate, E, Zero);
  SymRef Match = Ctx.mkIte(Gate, Ctx.mkConst(Expected), Zero);
  SymRef Miss = Ctx.mkIte(
      Gate, Ctx.mkConst(Expected ^ llvm::APInt(Expected.getBitWidth(), 1)),
      Zero);
  ASSERT_TRUE(referenceAgree(Ctx, Actual, Match, Sample + 1));
  ASSERT_FALSE(referenceAgree(Ctx, Actual, Miss, Sample + 1));
  EXPECT_TRUE(detail::agreeOnSamples(Ctx, Actual, Match, Sample + 1));
  EXPECT_TRUE(detail::agreeOnSamples(Ctx, Match, Actual, Sample + 1));
  EXPECT_FALSE(detail::agreeOnSamples(Ctx, Actual, Miss, Sample + 1));
  EXPECT_FALSE(detail::agreeOnSamples(Ctx, Miss, Actual, Sample + 1));
}

TEST(SymMBASample, ZeroSamplesDoNotRejectDifferentValues) {
  for (unsigned W : {1u, 8u, 32u, 64u, 65u, 128u, 257u}) {
    SCOPED_TRACE(W);
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", W);
    EXPECT_TRUE(detail::agreeOnSamples(Ctx, X, Ctx.mkNot(X), 0));
    EXPECT_TRUE(detail::agreeOnSamples(Ctx, Ctx.mkZero(W), Ctx.mkOnes(W), 0));
    EXPECT_FALSE(detail::agreeOnSamples(Ctx, Ctx.mkZero(W), Ctx.mkOnes(W), 1));
  }
}

TEST(SymMBASample, VisitsZeroOnesOneThenTheFirstRandomPoint) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 64);
  const auto Values = referenceAssignments(Ctx, 4);
  for (unsigned Sample = 0; Sample < Values.size(); ++Sample) {
    SCOPED_TRACE(Sample);
    for (unsigned Earlier = 0; Earlier < Sample; ++Earlier)
      ASSERT_NE(Values[Earlier][0], Values[Sample][0]);
    SymRef Hit = Ctx.mkEq(X, Ctx.mkConst(Values[Sample][0]));
    EXPECT_TRUE(detail::agreeOnSamples(Ctx, Hit, Ctx.mkFalse(), Sample));
    EXPECT_FALSE(detail::agreeOnSamples(Ctx, Hit, Ctx.mkFalse(), Sample + 1));
  }
}

TEST(SymMBASample, MatchesAPAcrossNarrowAndWideWidths) {
  for (unsigned W : {1u, 8u, 32u, 64u, 65u, 128u, 257u}) {
    SCOPED_TRACE(W);
    SymContext Ctx;
    SymRef Selector = Ctx.mkVar("selector", 64);
    SymRef X = Ctx.mkVar("x", W), Y = Ctx.mkVar("y", W);
    SymRef E = Ctx.mkXor(Ctx.mkMul(X, Y), Ctx.mkAdd(X, Y));
    for (unsigned Sample : {0u, 1u, 2u, 3u, 6u})
      expectAtSample(Ctx, E, Selector, Sample);
    SymRef Other = Ctx.mkOr(X, Y);
    for (unsigned Samples : {0u, 1u, 2u, 3u, 4u, 7u})
      EXPECT_EQ(detail::agreeOnSamples(Ctx, E, Other, Samples),
                referenceAgree(Ctx, E, Other, Samples));
  }
}

TEST(SymMBASample, UnusedWideVariablesStillConsumeTheirRandomWords) {
  SymContext Ctx;
  SymRef UnusedBefore = Ctx.mkVar("unused_before", 257);
  SymRef X = Ctx.mkVar("x", 64);
  SymRef UnusedBetween = Ctx.mkVar("unused_between", 65);
  SymRef Y = Ctx.mkVar("y", 32);
  SymRef UnusedAfter = Ctx.mkVar("unused_after", 128);
  const auto Values = referenceAssignments(Ctx, 4);
  for (SymRef Unused : {UnusedBefore, UnusedAfter})
    ASSERT_GT(Values.back()[Ctx.varId(Unused)].getActiveBits(), 64u);
  ASSERT_EQ(Ctx.width(UnusedBetween), 65u);
  SymRef Hit = Ctx.mkAnd(Ctx.mkEq(X, Ctx.mkConst(Values.back()[Ctx.varId(X)])),
                         Ctx.mkEq(Y, Ctx.mkConst(Values.back()[Ctx.varId(Y)])));
  ASSERT_TRUE(SymEvalPlan(Ctx, Hit).fitsU64());
  EXPECT_TRUE(detail::agreeOnSamples(Ctx, Hit, Ctx.mkFalse(), 3));
  EXPECT_FALSE(detail::agreeOnSamples(Ctx, Hit, Ctx.mkFalse(), 4));
  expectAtSample(Ctx, Ctx.mkXor(X, Ctx.mkZExt(Y, 64)), X, 6);
}

TEST(SymMBASample, ChecksInternalWidthsAndBothPlanOrientations) {
  for (unsigned W : {65u, 128u, 257u}) {
    SCOPED_TRACE(W);
    SymContext NarrowCtx;
    SymRef Selector = NarrowCtx.mkVar("selector", 64);
    SymRef X = NarrowCtx.mkVar("x", 8);
    SymRef Wide =
        NarrowCtx.mkUDiv(NarrowCtx.mkZExt(X, W), NarrowCtx.mkConst(W, 3));
    SymRef Low = NarrowCtx.mkExtract(Wide, 0, 8);
    ASSERT_FALSE(SymEvalPlan(NarrowCtx, Low).fitsU64());
    for (unsigned Sample : {0u, 1u, 2u, 3u})
      expectAtSample(NarrowCtx, Low, Selector, Sample);
  }
  SymContext Ctx;
  SymRef Selector = Ctx.mkVar("selector", 64);
  SymRef X = Ctx.mkVar("x", 8), Count = Ctx.mkVar("count", 32);
  SymRef Wide = Ctx.mkVar("wide", 257), WideY = Ctx.mkVar("wide_y", 257);
  SymRef Narrow = Ctx.mkVar("narrow", 1);
  const std::vector<SymRef> Exprs = {
      Ctx.mkExtract(Ctx.mkUDiv(Wide, WideY), 129, 8),
      Ctx.mkUlt(Wide, WideY),
      Ctx.mkShl(X, Wide),
      Ctx.mkZExt(X, 65),
      Ctx.mkSExt(X, 128),
      Ctx.mkConcat(Wide, Narrow),
      Ctx.mkLShr(X, Count)};
  for (size_t I = 0; I < Exprs.size(); ++I) {
    SCOPED_TRACE(I);
    EXPECT_EQ(SymEvalPlan(Ctx, Exprs[I]).fitsU64(), I + 1 == Exprs.size());
    for (unsigned Sample : {0u, 1u, 2u, 3u})
      expectAtSample(Ctx, Exprs[I], Selector, Sample);
  }
}

TEST(SymMBASample, MatchesAPOperatorSemanticsAtCornersAndRandomValues) {
  for (unsigned W : {8u, 64u, 128u}) {
    SCOPED_TRACE(W);
    SymContext Ctx;
    SymRef Selector = Ctx.mkVar("selector", 64);
    SymRef X = Ctx.mkVar("x", W), Y = Ctx.mkVar("y", W);
    SymRef Small = Ctx.mkVar("small", W / 2);
    SymRef SmallY = Ctx.mkVar("small_y", W / 2);
    SymRef Cond = Ctx.mkVar("cond", 1);
    const std::vector<std::pair<SymOp, SymRef>> Exprs = {
        {SymOp::Const, Ctx.mkConst(llvm::APInt::getSignedMinValue(W))},
        {SymOp::Var, X},
        {SymOp::Add, Ctx.mkAdd(X, Y)},
        {SymOp::Mul, Ctx.mkMul(X, Y)},
        {SymOp::And, Ctx.mkAnd(X, Y)},
        {SymOp::Or, Ctx.mkOr(X, Y)},
        {SymOp::Xor, Ctx.mkXor(X, Y)},
        {SymOp::Not, Ctx.mkNot(X)},
        {SymOp::Shl, Ctx.mkShl(X, Y)},
        {SymOp::LShr, Ctx.mkLShr(X, Y)},
        {SymOp::AShr, Ctx.mkAShr(X, Y)},
        {SymOp::UDiv, Ctx.mkUDiv(X, Y)},
        {SymOp::SDiv, Ctx.mkSDiv(X, Y)},
        {SymOp::URem, Ctx.mkURem(X, Y)},
        {SymOp::SRem, Ctx.mkSRem(X, Y)},
        {SymOp::Rol, Ctx.mkRol(X, Y)},
        {SymOp::Ror, Ctx.mkRor(X, Y)},
        {SymOp::Extract, Ctx.mkExtract(X, 1, W / 2)},
        {SymOp::Concat, Ctx.mkConcat(Small, SmallY)},
        {SymOp::ZExt, Ctx.mkZExt(Small, W)},
        {SymOp::SExt, Ctx.mkSExt(Small, W)},
        {SymOp::Ite, Ctx.mkIte(Cond, X, Y)},
        {SymOp::Eq, Ctx.mkEq(X, Y)},
        {SymOp::Ult, Ctx.mkUlt(X, Y)},
        {SymOp::Ule, Ctx.mkUle(X, Y)},
        {SymOp::Slt, Ctx.mkSlt(X, Y)},
        {SymOp::Sle, Ctx.mkSle(X, Y)}};
    for (const auto &[Op, E] : Exprs) {
      SCOPED_TRACE(unsigned(Op));
      ASSERT_EQ(Ctx.op(E), Op);
      for (unsigned Sample : {0u, 1u, 2u, 3u})
        expectAtSample(Ctx, E, Selector, Sample);
    }
    // The shared evaluator also defines division by zero and signed overflow.
    SymRef Min = Ctx.mkOr(X, Ctx.mkConst(llvm::APInt::getSignedMinValue(W)));
    SymRef NegativeOne = Ctx.mkNot(Y);
    expectAtSample(Ctx, Ctx.mkSDiv(Min, NegativeOne), Selector, 0);
    expectAtSample(Ctx, Ctx.mkSRem(Min, NegativeOne), Selector, 0);
    expectAtSample(Ctx, Ctx.mkUDiv(X, Y), Selector, 0);
    expectAtSample(Ctx, Ctx.mkURem(X, Y), Selector, 0);
  }
}

} // namespace
