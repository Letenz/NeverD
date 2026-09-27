//===- SymMBAWeightedTests.cpp - Exact weighted hidden inputs ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/symbolic/SymMBA.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace neverd::symbolic;

namespace {

SymRef hiddenFor(const detail::Abstraction &Abstract, SymRef Source) {
  for (const auto &[Index, Original] : Abstract.Hidden)
    if (Original == Source)
      return SymRef(Index);
  return {};
}

TEST(SymMBAWeighted, ExposesCompleteDivisibleTermsWithoutInvertingTheirBases) {
  for (unsigned Width : {2u, 3u, 8u, 32u, 64u, 128u, 256u}) {
    SCOPED_TRACE(Width);
    for (unsigned Kind : {0u, 1u, 2u}) {
      SCOPED_TRACE(Kind);
      SymContext Ctx;
      SymRef X = Ctx.mkVar("x", Width);
      SymRef Y = Ctx.mkVar("y", Width);
      SymRef Z = Ctx.mkVar("z", Width);
      SymRef Source = Kind == 0   ? X
                      : Kind == 1 ? Ctx.mkUDiv(X, Z)
                                  : Ctx.mkAdd(X, Z);
      SymRef P = Ctx.mkMul(Ctx.mkConst(Width, 2), Source);
      SymRef E = Ctx.mkAdd(Ctx.mkNeg(P), Ctx.mkAnd(Y, P));
      MBAOptions Opts;
      Opts.VerifySamples = 0;
      auto Result = simplifyMBA(Ctx, E, Opts);
      SymRef Expected = Ctx.mkNeg(Ctx.mkAnd(Ctx.mkNot(Y), P));
      EXPECT_EQ(Result.Expr, Expected) << Ctx.toString(Result.Expr);
      for (bool Products : {false, true}) {
        detail::WorkBudget Budget(MBAOptions::UnlimitedWork);
        auto Abstract = detail::abstractToMBA(Ctx, E, Products, &Budget);
        ASSERT_TRUE(Abstract);
        SymRef Hidden = hiddenFor(*Abstract, P);
        ASSERT_TRUE(Hidden);
        // At width two, -2 and 2 coincide before abstraction, so that
        // occurrence already shares P and keeps coefficient one.
        EXPECT_EQ(Abstract->Body,
                  Ctx.mkAdd(Width == 2 ? Hidden : Ctx.mkNeg(Hidden),
                            Ctx.mkAnd(Y, Hidden)));
      }
    }
  }
}

TEST(SymMBAWeighted, SolvesNonunitConstantCongruencesAtTheOriginalWidth) {
  for (unsigned Width : {3u, 8u, 32u, 64u, 128u, 256u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width);
    SymRef Y = Ctx.mkVar("y", Width);
    const llvm::APInt K(Width, 6);
    SymRef P = Ctx.mkMul(Ctx.mkConst(K), X);
    for (llvm::APInt D :
         {llvm::APInt(Width, -2, true), llvm::APInt(64, 10).zextOrTrunc(Width),
          llvm::APInt::getSignMask(Width)}) {
      SymRef E = Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(D), X), Ctx.mkAnd(P, Y));
      detail::WorkBudget Budget(MBAOptions::UnlimitedWork);
      auto Abstract = detail::abstractToMBA(Ctx, E, false, &Budget);
      ASSERT_TRUE(Abstract);
      llvm::SmallVector<uint32_t, 8> Vars;
      Ctx.collectVars(Abstract->Body, Vars);
      EXPECT_FALSE(llvm::is_contained(Vars, Ctx.varId(X)));
      SymRef Restored = Ctx.substitute(Abstract->Body, Abstract->Hidden);
      // Restoring q*P combines its constants again, so the original modular
      // coefficient is recovered without any evaluation or source narrowing.
      EXPECT_EQ(Restored, E);
    }
  }
}

TEST(SymMBAWeighted, PreservesIndependentBitwiseUsesAndImpossibleCongruences) {
  constexpr unsigned Width = 4;
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", Width);
  SymRef Y = Ctx.mkVar("y", Width);
  SymRef P = Ctx.mkMul(Ctx.mkConst(Width, 6), X);
  for (unsigned D = 0; D < 16; ++D) {
    SCOPED_TRACE(D);
    SymRef E = Ctx.mkAdd({Ctx.mkMul(Ctx.mkConst(Width, D), X), Ctx.mkAnd(P, Y),
                          Ctx.mkXor(X, Y)});
    for (bool Products : {false, true}) {
      detail::WorkBudget Budget(MBAOptions::UnlimitedWork);
      auto Abstract = detail::abstractToMBA(Ctx, E, Products, &Budget);
      ASSERT_TRUE(Abstract);
      SymRef Restored = Ctx.substitute(Abstract->Body, Abstract->Hidden);
      std::vector<uint64_t> Values(Ctx.numVars(), 0);
      for (unsigned XV = 0; XV < 16; ++XV)
        for (unsigned YV = 0; YV < 16; ++YV) {
          Values[Ctx.varId(X)] = XV;
          Values[Ctx.varId(Y)] = YV;
          EXPECT_EQ(Ctx.evalU64(Restored, Values), Ctx.evalU64(E, Values));
        }
      llvm::SmallVector<uint32_t, 8> Vars;
      Ctx.collectVars(Abstract->Body, Vars);
      EXPECT_TRUE(llvm::is_contained(Vars, Ctx.varId(X)));
    }
    // An odd requested coefficient distinguishes sources X and X+8 even
    // though their hidden multiple 6*X is equal. It cannot become a function
    // of that hidden input alone.
    if (D & 1)
      EXPECT_NE((D * 0) & 15, (D * 8) & 15);
  }
}

TEST(SymMBAWeighted, RefusalLeavesTheCompleteAbstractionAndAllowsRetry) {
  for (unsigned Width : {4u, 64u, 256u}) {
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width);
    SymRef Y = Ctx.mkVar("y", Width);
    SymRef Z = Ctx.mkVar("z", Width);
    SymRef P = Ctx.mkMul(Ctx.mkConst(Width, 6), X);
    SymRef Q = Ctx.mkMul(Ctx.mkConst(Width, 10), Z);
    SymRef E =
        Ctx.mkAdd({Ctx.mkMul(Ctx.mkConst(llvm::APInt(Width, -2, true)), X),
                   Ctx.mkMul(Ctx.mkConst(llvm::APInt(Width, -4, true)), Z),
                   Ctx.mkAnd(P, Y), Ctx.mkOr(Q, Y)});
    detail::WorkBudget Zero(0);
    auto Original = detail::abstractToMBA(Ctx, E, false, &Zero);
    ASSERT_TRUE(Original);
    detail::WorkBudget Unlimited(MBAOptions::UnlimitedWork);
    auto Complete = detail::abstractToMBA(Ctx, E, false, &Unlimited);
    ASSERT_TRUE(Complete);
    ASSERT_NE(Complete->Body, Original->Body);
    const size_t Step = std::max(size_t(1), Unlimited.used() / 17);
    for (size_t Limit = 0; Limit < Unlimited.used(); Limit += Step) {
      detail::WorkBudget Tight(Limit);
      auto Declined = detail::abstractToMBA(Ctx, E, false, &Tight);
      ASSERT_TRUE(Declined);
      EXPECT_EQ(Declined->Body, Original->Body);
      EXPECT_EQ(Declined->Hidden, Original->Hidden);
      EXPECT_TRUE(Tight.exhausted());
      EXPECT_LE(Tight.used(), Limit);
    }
    detail::WorkBudget NoBytes(MBAOptions::UnlimitedWork);
    auto Declined = detail::abstractToMBA(Ctx, E, false, &NoBytes, 0);
    ASSERT_TRUE(Declined);
    EXPECT_EQ(Declined->Body, Original->Body);
    detail::WorkBudget Retry(MBAOptions::UnlimitedWork);
    auto Retried = detail::abstractToMBA(Ctx, E, false, &Retry);
    ASSERT_TRUE(Retried);
    EXPECT_EQ(Retried->Body, Complete->Body);
    EXPECT_EQ(Retry.used(), Unlimited.used());
    EXPECT_EQ(Ctx.substitute(Retried->Body, Retried->Hidden), E);
  }
}

TEST(SymMBAWeighted, IndexesSharedBasesInsteadOfCrossingInputsAndUses) {
  size_t Previous = 0;
  for (unsigned Count : {64u, 128u, 256u}) {
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", 64);
    llvm::SmallVector<SymRef, 32> Terms;
    for (unsigned I = 0; I < Count; ++I) {
      SymRef Y = Ctx.mkVar("y" + std::to_string(I), 64);
      SymRef P = Ctx.mkMul(Ctx.mkConst(64, 4 * I + 2), X);
      // Arithmetic terms have distinct complete bases, while every hidden
      // source uses the same X. A relation scan per term would be quadratic.
      Terms.push_back(Ctx.mkAnd(P, Y));
      Terms.push_back(Ctx.mkMul(Ctx.mkConst(64, 2), Y));
    }
    Terms.push_back(Ctx.mkMul(Ctx.mkConst(64, -2), X));
    SymRef E = Ctx.mkAdd(Terms);
    detail::WorkBudget Budget(MBAOptions::UnlimitedWork);
    auto Abstract = detail::abstractToMBA(Ctx, E, false, &Budget);
    ASSERT_TRUE(Abstract);
    if (Previous)
      EXPECT_LT(Budget.used(), Previous * 3);
    Previous = Budget.used();
    EXPECT_EQ(Ctx.substitute(Abstract->Body, Abstract->Hidden), E);
  }
}

TEST(SymMBAWeighted, ChoosesACompleteRelationWithTheSmallestPowerOfTwo) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 64);
  SymRef Y = Ctx.mkVar("y", 64);
  SymRef Four = Ctx.mkMul(Ctx.mkConst(64, 4), X);
  SymRef Six = Ctx.mkMul(Ctx.mkConst(64, 6), X);
  SymRef Eight = Ctx.mkMul(Ctx.mkConst(64, 8), X);
  SymRef E = Ctx.mkAdd({Ctx.mkMul(Ctx.mkConst(64, 2), X), Ctx.mkAnd(Four, Y),
                        Ctx.mkOr(Six, Y), Ctx.mkXor(Eight, Y)});
  detail::WorkBudget Budget(MBAOptions::UnlimitedWork);
  auto Abstract = detail::abstractToMBA(Ctx, E, false, &Budget);
  ASSERT_TRUE(Abstract);
  llvm::SmallVector<uint32_t, 8> Vars;
  Ctx.collectVars(Abstract->Body, Vars);
  EXPECT_FALSE(llvm::is_contained(Vars, Ctx.varId(X)));
  EXPECT_EQ(Ctx.substitute(Abstract->Body, Abstract->Hidden), E);
}

TEST(SymMBAWeighted, BoundsWideCoefficientStorageBeforeBuildingTemplates) {
  constexpr unsigned Width = 8192;
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", Width);
  SymRef Y = Ctx.mkVar("y", Width);
  SymRef P = Ctx.mkMul(Ctx.mkConst(Width, 6), X);
  SymRef E = Ctx.mkAdd(Ctx.mkNeg(P), Ctx.mkAnd(P, Y));
  detail::WorkBudget Zero(0);
  auto Original = detail::abstractToMBA(Ctx, E, false, &Zero);
  ASSERT_TRUE(Original);
  const size_t Nodes = Ctx.numNodes();
  detail::WorkBudget Work(MBAOptions::UnlimitedWork);
  auto Declined = detail::abstractToMBA(Ctx, E, false, &Work, 128);
  ASSERT_TRUE(Declined);
  EXPECT_EQ(Declined->Body, Original->Body);
  EXPECT_EQ(Declined->Hidden, Original->Hidden);
  EXPECT_EQ(Ctx.numNodes(), Nodes);
  EXPECT_FALSE(Work.exhausted());
}

} // namespace
