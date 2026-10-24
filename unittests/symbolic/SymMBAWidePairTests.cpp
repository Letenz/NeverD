//===- SymMBAWidePairTests.cpp - Exact two-bit product correction tests ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independently check variable two-bit corrections, mixed masks, normalized
/// products, nearby noncongruences and cumulative resource refusal.
///
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/solver/SymSynthVerifier.h"
#include "neverd/solver/Z3Solver.h"

#include <array>
#include <random>
#include <utility>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

llvm::APInt power(llvm::APInt Value, unsigned Exponent) {
  llvm::APInt Result(Value.getBitWidth(), 1);
  for (; Exponent; Exponent >>= 1) {
    if (Exponent & 1)
      Result *= Value;
    Value *= Value;
  }
  return Result;
}

struct Fixture {
  SymRef Input;
  unsigned Width, Kind;
  bool Neighbor;

  llvm::APInt oracle(const std::array<llvm::APInt, 3> &V) const {
    auto Factor = [&](bool Shifted) {
      auto X = V[0] + (Shifted ? 64 + unsigned(Neighbor) : 0);
      auto Y = V[1] + (Shifted ? (Kind == 3 ? 32 : 64) : 0);
      auto Z = V[2] + (Shifted ? (Kind == 3 ? 32 : 128) : 0);
      const auto Mask = ~llvm::APInt(Width, 3);
      auto High = Y & Mask;
      if (Kind == 1 || Kind == 2)
        High = Y & llvm::APInt(Width, Kind == 1 ? 0x7c : 0xbc);
      else if (Kind == 3)
        High = (Y & ~llvm::APInt(Width, 1)) * (Z & ~llvm::APInt(Width, 1));
      else if (Kind == 4)
        High = ((Y + 7) & ~llvm::APInt(Width, 7)) + (Z & Mask);
      if (Kind == 5)
        return X ^ High ^ (Z & Mask) ^ llvm::APInt(Width, Shifted ? 128 : 64);
      return Kind == 6 ? X + High : X ^ High;
    };
    return llvm::APInt(Width, 3).shl(Width - 8) * power(Factor(false), 63) *
           power(Factor(true), 65);
  }
};

Fixture fixture(SymContext &Ctx, unsigned Width, unsigned Kind,
                bool Neighbor = false) {
  auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
  auto Z = Ctx.mkVar("z", Width);
  auto C = [&](uint64_t V) { return Ctx.mkConst(Width, V); };
  auto Mask = Ctx.mkConst(~llvm::APInt(Width, 3));
  auto Factor = [&](bool Shifted) {
    auto A = Ctx.mkAdd(X, C(Shifted ? 64 + unsigned(Neighbor) : 0));
    auto B = Ctx.mkAdd(Y, C(Shifted ? (Kind == 3 ? 32 : 64) : 0));
    auto D = Ctx.mkAdd(Z, C(Shifted ? (Kind == 3 ? 32 : 128) : 0));
    auto High = Ctx.mkAnd(B, Mask);
    if (Kind == 1 || Kind == 2)
      High = Ctx.mkAnd(B, C(Kind == 1 ? 0x7c : 0xbc));
    else if (Kind == 3) {
      auto Even = Ctx.mkConst(~llvm::APInt(Width, 1));
      High = Ctx.mkMul(Ctx.mkAnd(B, Even), Ctx.mkAnd(D, Even));
    } else if (Kind == 4)
      High = Ctx.mkAdd(
          Ctx.mkAnd(Ctx.mkAdd(B, C(7)), Ctx.mkConst(~llvm::APInt(Width, 7))),
          Ctx.mkAnd(D, Mask));
    if (Kind == 5)
      return Ctx.mkXor({A, High, Ctx.mkAnd(D, Mask), C(Shifted ? 128 : 64)});
    return Kind == 6 ? Ctx.mkAdd(A, High) : Ctx.mkXor(A, High);
  };
  auto A = Factor(false), B = Factor(true);
  llvm::SmallVector<SymRef, 128> Factors(63, A);
  Factors.append(65, B);
  Factors.push_back(Ctx.mkConst(llvm::APInt(Width, 3).shl(Width - 8)));
  return {Ctx.mkMul(Factors), Width, Kind, Neighbor};
}

TEST(SymMBAWidePairs, CorrectionsPreserveMasksProductsAndXorCrossTerms) {
  std::mt19937_64 Random(0x57494445434f5252);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Kind = 0; Kind < 7; ++Kind)
      for (bool Neighbor : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << Width << ':' << Kind << ':' << Neighbor);
        SymContext Ctx;
        auto F = fixture(Ctx, Width, Kind, Neighbor);
        detail::WorkBudget Budget(65536);
        size_t Storage = 1 << 24;
        auto Result =
            detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
        if (!Neighbor)
          ASSERT_NE(Result, F.Input);
        EXPECT_LE(Budget.used(), 65536u);
        SymEvalPlan Reduced(Ctx, Result);
        for (unsigned Trial = 0; Trial < 512; ++Trial) {
          std::array<llvm::APInt, 3> Values;
          for (auto &V : Values) {
            V = llvm::APInt(Width, Random());
            for (unsigned Bit = 64; Bit < Width; Bit += 64)
              V |= llvm::APInt(Width, Random()).shl(Bit);
          }
          if (Trial < 256)
            Values = {llvm::APInt(Width, Trial), llvm::APInt(Width, 3 * Trial),
                      llvm::APInt(Width, 7 * Trial)};
          ASSERT_EQ(Reduced.eval(Values), F.oracle(Values));
        }
      }
}

TEST(SymMBAWidePairs, ExhaustsNarrowCorrectionsAndTheirCarryBoundaries) {
  for (unsigned Kind = 0; Kind < 7; ++Kind) {
    SCOPED_TRACE(Kind);
    SymContext Ctx;
    auto F = fixture(Ctx, 8, Kind);
    detail::WorkBudget Budget(65536);
    size_t Storage = 1 << 24;
    auto Result = detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
    ASSERT_NE(Result, F.Input);
    SymEvalPlan Reduced(Ctx, Result);
    for (unsigned X = 0; X < 256; ++X)
      for (unsigned Y = 0; Y < 256; ++Y) {
        const std::array<llvm::APInt, 3> Values{
            llvm::APInt(8, X), llvm::APInt(8, Y), llvm::APInt(8, X + 3 * Y)};
        ASSERT_EQ(Reduced.eval(Values), F.oracle(Values)) << X << ':' << Y;
      }
  }
}

TEST(SymMBAWidePairs, CorrectionsHaveIndependentFullWidthProofs) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {8u, 16u, 32u, 64u})
    for (unsigned Kind : {0u, 1u, 2u, 3u, 5u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Kind);
      SymContext Ctx;
      auto F = fixture(Ctx, Width, Kind);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      auto Result = detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
      ASSERT_NE(Result, F.Input);
      // This independent 128-factor proof has a wall-clock timeout. Leave
      // room for shared CI runners while still requiring an unsat result;
      // production simplification and its resource limits are unchanged.
      constexpr unsigned ProofTimeoutMs = 30000;
      solver::Z3SolverOptions Options;
      Options.TimeoutMs = ProofTimeoutMs;
      solver::Z3Solver Verifier(Ctx, Options);
      // Each verifier checks one complete formula, allowing one-shot
      // preprocessing instead of treating the difference as an assumption.
      ASSERT_TRUE(Verifier.assertDistinct(F.Input, Result));
      EXPECT_EQ(Verifier.check(), solver::SatResult::Unsat)
          << Verifier.reasonUnknown();
    }
}

TEST(SymMBAWidePairs, RefusalsKeepTheFullWordAndCumulativeBounds) {
  for (auto Limits : {std::pair<size_t, size_t>{0, 1 << 24},
                      {1, 1 << 24},
                      {8192, 1 << 24},
                      {65536, 0},
                      {65536, 256},
                      {65536, 1 << 24}}) {
    SymContext Ctx;
    auto F = fixture(Ctx, 64, 3);
    detail::WorkBudget Budget(Limits.first);
    size_t Storage = Limits.second;
    auto Result = detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
    EXPECT_LE(Budget.used(), Limits.first);
    EXPECT_LE(Storage, Limits.second);
    SymEvalPlan Reduced(Ctx, Result);
    for (unsigned I = 0; I < 256; ++I) {
      const std::array<llvm::APInt, 3> Values{
          llvm::APInt(64, I), llvm::APInt(64, I * 17), llvm::APInt(64, I * 51)};
      EXPECT_EQ(Reduced.eval(Values), F.oracle(Values));
    }
  }
}

TEST(SymMBAWidePairs, QuadraticCarryCountsPreserveAllObservedBits) {
  std::mt19937_64 Random(0x51554144434f554e);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Observed : {6u, 7u, 8u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Observed);
      SymContext Ctx;
      auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
      const std::array Parts{X, Y};
      const unsigned Split = (Observed + 2) / 3;
      const auto Scale = llvm::APInt(Width, 3).shl(Width - Observed);
      llvm::SmallVector<SymRef, 32> Factors{Ctx.mkConst(Scale)};
      // Odd multiplicities retain both correction bits. Distinct offsets keep
      // this a larger source product rather than repeated identical factors.
      for (unsigned Copy = 0; Copy < 23; ++Copy)
        for (unsigned I = 0; I < Parts.size(); ++I)
          Factors.push_back(Ctx.mkAdd(
              Ctx.mkConst(Width, 1),
              Ctx.mkMul(
                  Ctx.mkConst(Width, uint64_t(1) << Split),
                  Ctx.mkAdd(Parts[I],
                            Ctx.mkConst(Width, Copy * Parts.size() + I)))));
      auto Input = Ctx.mkMul(Factors);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      auto Result = detail::foldResidueDecisions(Ctx, Input, Budget, Storage);
      ASSERT_NE(Result, Input) << "work=" << Budget.used()
                               << " cost=" << detail::readingCost(Ctx, Input);
      SymEvalPlan Reduced(Ctx, Result);
      for (unsigned Trial = 0; Trial < 512; ++Trial) {
        std::array<llvm::APInt, 2> V;
        for (auto &Value : V) {
          Value = llvm::APInt(Width, Random());
          for (unsigned Bit = 64; Bit < Width; Bit += 64)
            Value |= llvm::APInt(Width, Random()).shl(Bit);
        }
        // Exhaust both low residues independently, then retain random
        // complete words to catch accidental narrowing of the scaled result.
        if (Trial < 16)
          V = {llvm::APInt(Width, Trial & 3),
               llvm::APInt(Width, (Trial >> 2) & 3)};
        const auto &Values = V;
        auto Expected = Scale;
        for (unsigned Copy = 0; Copy < 23; ++Copy)
          for (unsigned I = 0; I < Values.size(); ++I)
            Expected *= llvm::APInt(Width, 1) +
                        (Values[I] + Copy * Values.size() + I).shl(Split);
        EXPECT_EQ(Reduced.eval(V), Expected);
      }
      if (Width <= 64 &&
          solver::proofBackendAvailable(solver::ProofBackend::Z3)) {
        solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 10000);
        EXPECT_EQ(Verifier(Ctx, Input, Result), SynthVerification::Equivalent);
      }
    }
}

} // namespace
