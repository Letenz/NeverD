//===- SymMBAResiduePowerTests.cpp - Bounded modular power decisions -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Check high-degree low-word products against independently multiplied
/// complete words, including even inputs and exhausted proof resources.
///
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/solver/SymSynthVerifier.h"
#include "neverd/solver/Z3Solver.h"
#include "neverd/symbolic/SymMBA.h"

#include <array>
#include <random>
#include <utility>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

struct PowerFixture {
  SymRef Input;
  unsigned Width, Observed, Count;

  llvm::APInt oracle(const std::array<llvm::APInt, 3> &Values) const {
    auto Result = llvm::APInt(Width, 3).shl(Width - Observed) * Values[2];
    for (unsigned I = 0; I < Count; ++I)
      Result *= (Values[0] + llvm::APInt(Width, I).shl((Observed + 2) / 3)) ^
                Values[1].shl((Observed + 2) / 3);
    return Result;
  }
};

PowerFixture fixture(SymContext &Ctx, unsigned Width, unsigned Observed,
                     unsigned Count) {
  const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
  const auto Z = Ctx.mkVar("z", Width);
  const unsigned Split = (Observed + 2) / 3;
  const auto High = Ctx.mkShl(Y, Ctx.mkConst(Width, Split));
  llvm::SmallVector<SymRef, 128> Factors{
      Ctx.mkConst(llvm::APInt(Width, 3).shl(Width - Observed)), Z};
  for (unsigned I = 0; I < Count; ++I)
    Factors.push_back(Ctx.mkXor(
        Ctx.mkAdd(X, Ctx.mkConst(llvm::APInt(Width, I).shl(Split))), High));
  // Z has independent parity. In particular, an odd Z cannot justify
  // treating the repeated X factors as odd before reducing their powers.
  return {Ctx.mkMul(Factors), Width, Observed, Count};
}

TEST(SymMBAResiduePowers, LowWordsKeepEvenInputsAndSmallExponents) {
  for (unsigned Observed = 1; Observed <= 8; ++Observed)
    for (unsigned Count :
         {1u, 2u, 3u, 7u, 8u, 16u, 17u, 32u, 33u, 64u, 65u, 96u}) {
      SCOPED_TRACE(::testing::Message() << Observed << ':' << Count);
      SymContext Ctx;
      const auto F = fixture(Ctx, 8, Observed, Count);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      const auto Result =
          detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
      EXPECT_LE(Budget.used(), 65536u);
      SymEvalPlan Reduced(Ctx, Result);
      for (unsigned Trial = 0; Trial < 1024; ++Trial) {
        const std::array Values{
            llvm::APInt(8, Trial), llvm::APInt(8, Trial / 4),
            llvm::APInt(8, std::array{0u, 1u, 2u, 255u}[Trial / 256])};
        EXPECT_EQ(Reduced.eval(Values), F.oracle(Values)) << Trial;
      }
    }
}

TEST(SymMBAResiduePowers, LargeGroupsRetainFullWidthWords) {
  std::mt19937_64 Random(0x504f574552574944);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Count : {64u, 65u, 96u, 128u, 192u, 256u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Count);
      SymContext Ctx;
      const auto F = fixture(Ctx, Width, 8, Count);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      const auto Result =
          detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
      // The immediate odd neighbor retains a nonconstant high correction;
      // it is an equivalence control even when the bounded emitter declines.
      if (Count != 65) {
        ASSERT_NE(Result, F.Input) << "work=" << Budget.used();
        EXPECT_LT(detail::readingCost(Ctx, Result),
                  detail::readingCost(Ctx, F.Input));
      }
      SymEvalPlan Reduced(Ctx, Result);
      for (unsigned Trial = 0; Trial < 128; ++Trial) {
        std::array<llvm::APInt, 3> Values;
        for (auto &Value : Values) {
          Value = llvm::APInt(Width, 0);
          for (unsigned Bit = 0; Bit < Width; Bit += 64)
            Value |= llvm::APInt(Width, Random()).shl(Bit);
        }
        EXPECT_EQ(Reduced.eval(Values), F.oracle(Values));
      }
    }
}

TEST(SymMBAResiduePowers, ExactAndShortResourcesKeepCompleteProofs) {
  SymContext FullCtx;
  const auto F = fixture(FullCtx, 64, 8, 64);
  detail::WorkBudget FullBudget(65536);
  size_t FullStorage = 1 << 24;
  const auto Full =
      detail::foldResidueDecisions(FullCtx, F.Input, FullBudget, FullStorage);
  ASSERT_NE(Full, F.Input);
  const auto Work = FullBudget.used();
  const size_t Bytes = (1 << 24) - FullStorage;
  ASSERT_GT(Work, 1u);
  ASSERT_GT(Bytes, 1u);
  for (auto Limits : {std::pair<size_t, size_t>{0, Bytes},
                      {1, Bytes},
                      {Work, 0},
                      {Work - 1, Bytes},
                      {Work, Bytes - 1},
                      {Work, Bytes}}) {
    SymContext Ctx;
    const auto Input = fixture(Ctx, 64, 8, 64);
    detail::WorkBudget Budget(Limits.first);
    size_t Storage = Limits.second;
    const auto Result =
        detail::foldResidueDecisions(Ctx, Input.Input, Budget, Storage);
    EXPECT_LE(Budget.used(), Limits.first);
    EXPECT_LE(Storage, Limits.second);
    if (Limits.first == Work && Limits.second == Bytes)
      EXPECT_EQ(Result.index(), Full.index());
    SymEvalPlan Reduced(Ctx, Result);
    for (unsigned I = 0; I < 256; ++I) {
      const std::array Values{llvm::APInt(64, I), llvm::APInt(64, I * 17),
                              llvm::APInt(64, I * 51)};
      EXPECT_EQ(Reduced.eval(Values), Input.oracle(Values));
    }
  }
}

TEST(SymMBAResiduePowers, ParitySubgroupsKeepIndependentMultipliers) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext ExactCtx;
    const auto ExactInput = fixture(ExactCtx, Width, 6, 64);
    detail::WorkBudget Budget(65536);
    size_t Storage = 1 << 24;
    const auto Exact = detail::foldResidueDecisions(ExactCtx, ExactInput.Input,
                                                    Budget, Storage, true);
    ASSERT_NE(Exact, ExactInput.Input);
    SymContext Ctx;
    const auto F = fixture(Ctx, Width, 6, 64);
    MBAOptions Options;
    Options.MaxWork = 65536;
    Options.VerifySamples = 0;
    const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
    ASSERT_TRUE(Result.Changed);
    EXPECT_LT(Result.Work, Options.MaxWork);
    EXPECT_LT(Result.SizeAfter, F.Count);
    SymEvalPlan Reduced(Ctx, Result.Expr);
    for (unsigned I = 0; I < 256; ++I) {
      const std::array Values{llvm::APInt(Width, I), llvm::APInt(Width, I * 17),
                              llvm::APInt(Width, I / 4)};
      EXPECT_EQ(Reduced.eval(Values), F.oracle(Values));
    }
  }
}

TEST(SymMBAResiduePowers, NativeWidthsHaveIndependentSolverProofs) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {8u, 16u, 32u, 64u})
    for (unsigned Count : {2u, 7u, 16u, 17u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Count);
      SymContext Ctx;
      const auto F = fixture(Ctx, Width, Count < 8 ? 8 : 4, Count);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      const auto Result =
          detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
      // A complete period must exercise an actual rewrite in each native
      // width; unchanged neighbor controls alone would make this proof vacuous.
      if (Count == 16)
        ASSERT_NE(Result, F.Input);
      solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 10000);
      EXPECT_EQ(Verifier(Ctx, F.Input, Result), SynthVerification::Equivalent);
    }
}

} // namespace
