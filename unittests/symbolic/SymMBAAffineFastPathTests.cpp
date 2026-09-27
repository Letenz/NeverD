//===- SymMBAAffineFastPathTests.cpp - Bounded affine relation search ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/symbolic/SymMBA.h"

#include <string>
#include <vector>

using namespace neverd::symbolic;

namespace {

SymRef affine(SymContext &Ctx, SymRef X, int64_t Scale, int64_t Offset) {
  unsigned Width = Ctx.width(X);
  auto literal = [&](int64_t V) {
    return Ctx.mkConst(llvm::APInt(64, V, true).sextOrTrunc(Width));
  };
  return Ctx.mkAdd(Ctx.mkMul(literal(Scale), X), literal(Offset));
}

TEST(SymMBAAffineFastPath, SameParitySharedInputsFitBoundedWork) {
  for (unsigned Parity : {0u, 1u}) {
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", 64);
    llvm::SmallVector<SymRef, 64> Terms;
    for (unsigned I = 0; I < 64; ++I) {
      SymRef Mask = Ctx.mkVar("mask" + std::to_string(I), 64);
      Terms.push_back(
          Ctx.mkAnd(affine(Ctx, X, 4 * I + 2, 2 * I + Parity), Mask));
    }
    SymRef E = Ctx.mkAdd(Terms);
    // All offsets have the same parity, which excludes affine complements.
    // The search should finish within this budget even with 64 shared bases.
    detail::WorkBudget Budget(5800);
    auto A = detail::abstractToMBA(Ctx, E, false, &Budget);
    ASSERT_TRUE(A);
    EXPECT_FALSE(Budget.exhausted());
    EXPECT_EQ(Ctx.substitute(A->Body, A->Hidden), E);
  }
}

TEST(SymMBAAffineFastPath, KeepsDependentAndNestedComplementSources) {
  for (unsigned Width : {3u, 8u, 32u, 64u, 128u, 256u}) {
    for (int64_t Scale : {2, 3, -6, -7}) {
      for (bool Products : {false, true}) {
        SymContext Ctx;
        SymRef X = Ctx.mkVar("x", Width);
        SymRef F = affine(Ctx, X, Scale, 1);
        SymRef G = Ctx.mkSub(Ctx.mkNeg(F), Ctx.mkOne(Width));
        // F and G's boundary summaries can both have odd offsets: G's
        // complete forced F term must retain the direct complement search.
        detail::WorkBudget Dependent(MBAOptions::UnlimitedWork);
        auto A =
            detail::abstractToMBA(Ctx, Ctx.mkAnd(F, G), Products, &Dependent);
        ASSERT_TRUE(A);
        EXPECT_EQ(A->Body, Ctx.mkZero(Width));

        SymRef Nested = affine(Ctx, Ctx.mkAdd(X, Ctx.mkOne(Width)), Scale, 0);
        SymRef Opposite = affine(Ctx, X, -Scale, -Scale - 1);
        detail::WorkBudget Other(MBAOptions::UnlimitedWork);
        auto B = detail::abstractToMBA(Ctx, Ctx.mkOr(Nested, Opposite),
                                       Products, &Other);
        ASSERT_TRUE(B);
        EXPECT_EQ(B->Body, Ctx.mkOnes(Width));
      }
    }
  }
}

TEST(SymMBAAffineFastPath, SameParityNeighboursPreserveActualRestoration) {
  constexpr unsigned Width = 3;
  for (int64_t Scale : {2, 3, -2, -3}) {
    for (int64_t Offset : {0, 1, 2, 3}) {
      SymContext Ctx;
      SymRef X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
      SymRef F = affine(Ctx, X, Scale, Offset);
      SymRef G = affine(Ctx, X, -Scale, Offset + 2);
      SymRef E = Ctx.mkAdd({Ctx.mkAnd(F, G), Ctx.mkOr(F, Y), X});
      detail::WorkBudget Budget(MBAOptions::UnlimitedWork);
      auto A = detail::abstractToMBA(Ctx, E, false, &Budget);
      ASSERT_TRUE(A);
      SymRef Restored = Ctx.substitute(A->Body, A->Hidden);
      std::vector<uint64_t> Values(Ctx.numVars(), 0);
      for (unsigned XV = 0; XV < 8; ++XV)
        for (unsigned YV = 0; YV < 8; ++YV) {
          Values[Ctx.varId(X)] = XV;
          Values[Ctx.varId(Y)] = YV;
          EXPECT_EQ(Ctx.evalU64(Restored, Values), Ctx.evalU64(E, Values));
        }
    }
  }
}

TEST(SymMBAAffineFastPath, GateBudgetRefusalRemainsAtomic) {
  for (unsigned Width : {8u, 64u, 256u}) {
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    SymRef F = affine(Ctx, X, 3, 1);
    SymRef G = Ctx.mkSub(Ctx.mkNeg(F), Ctx.mkOne(Width));
    SymRef E = Ctx.mkAdd({Ctx.mkAnd(F, G), Ctx.mkAnd(F, Y), X});
    detail::WorkBudget Zero(0), Unlimited(MBAOptions::UnlimitedWork);
    auto Original = detail::abstractToMBA(Ctx, E, false, &Zero);
    auto Full = detail::abstractToMBA(Ctx, E, false, &Unlimited);
    ASSERT_TRUE(Original);
    ASSERT_TRUE(Full);
    ASSERT_NE(Original->Body, Full->Body);
    for (size_t Limit = 0; Limit < Unlimited.used(); ++Limit) {
      detail::WorkBudget Tight(Limit);
      auto A = detail::abstractToMBA(Ctx, E, false, &Tight);
      ASSERT_TRUE(A);
      EXPECT_TRUE(Tight.exhausted());
      EXPECT_EQ(A->Body, Original->Body);
      EXPECT_EQ(A->Hidden, Original->Hidden);
    }
    for (size_t Bytes = 0; Bytes < 8192; Bytes += 113) {
      detail::WorkBudget Budget(MBAOptions::UnlimitedWork);
      auto A = detail::abstractToMBA(Ctx, E, false, &Budget, Bytes);
      ASSERT_TRUE(A);
      EXPECT_TRUE(A->Body == Original->Body || A->Body == Full->Body);
      EXPECT_EQ(A->Hidden, Original->Hidden);
    }
  }
}

} // namespace
