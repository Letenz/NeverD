//===- SymMBAComplementTests.cpp - Exact hidden input complements --------===//
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

SymRef scaledOffset(SymContext &Ctx, SymRef X, int64_t Scale, int64_t Offset) {
  unsigned Width = Ctx.width(X);
  return Ctx.mkAdd(
      Ctx.mkMul(Ctx.mkConst(llvm::APInt(64, Scale, true).sextOrTrunc(Width)),
                X),
      Ctx.mkConst(llvm::APInt(64, Offset, true).sextOrTrunc(Width)));
}

TEST(SymMBAComplement, IdentifiesOddAndEvenAffineComplements) {
  for (unsigned Width : {2u, 3u, 8u, 32u, 64u, 128u, 256u}) {
    SCOPED_TRACE(Width);
    for (int64_t Scale : {-6, -3, -1, 2, 3, 6}) {
      SCOPED_TRACE(Scale);
      for (unsigned Kind : {0u, 1u, 2u}) {
        SymContext Ctx;
        SymRef X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
        SymRef Source = Kind == 0   ? X
                        : Kind == 1 ? Ctx.mkUDiv(X, Y)
                                    : Ctx.mkXor(X, Y);
        SymRef F = scaledOffset(Ctx, Source, Scale, 5);
        SymRef G = scaledOffset(Ctx, Source, -Scale, -6);
        for (bool Products : {false, true}) {
          detail::WorkBudget Budget(MBAOptions::UnlimitedWork);
          auto A =
              detail::abstractToMBA(Ctx, Ctx.mkAnd(F, G), Products, &Budget);
          ASSERT_TRUE(A);
          EXPECT_EQ(A->Body, Ctx.mkZero(Width)) << Ctx.toString(A->Body);
          detail::WorkBudget Other(MBAOptions::UnlimitedWork);
          auto B = detail::abstractToMBA(Ctx, Ctx.mkOr(F, G), Products, &Other);
          ASSERT_TRUE(B);
          EXPECT_EQ(B->Body, Ctx.mkOnes(Width)) << Ctx.toString(B->Body);
        }
      }
    }
  }
}

TEST(SymMBAComplement, ExposesPartitionedProductsAndDependentSources) {
  for (unsigned Width : {8u, 32u, 64u, 128u}) {
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    SymRef Z = Ctx.mkVar("z", Width);
    for (SymRef Source :
         {Ctx.mkNeg(Y), Ctx.mkAdd(Y, Z), Ctx.mkAdd(Y, Ctx.mkMul(X, Z))}) {
      SymRef Opposite = Ctx.mkSub(Ctx.mkNeg(Source), Ctx.mkOne(Width));
      SymRef E = Ctx.mkAdd(
          Ctx.mkMul(Ctx.mkAnd(X, Source), Ctx.mkOr(X, Source)),
          Ctx.mkMul(Ctx.mkAnd(X, Opposite), Ctx.mkAnd(Ctx.mkNot(X), Source)));
      MBAOptions Options;
      Options.VerifySamples = 0;
      auto Result = simplifyMBA(Ctx, E, Options);
      EXPECT_EQ(Result.Expr, Ctx.mkMul(X, Source)) << Ctx.toString(Result.Expr);
    }
  }
}

TEST(SymMBAComplement, DoesNotIdentifyOffsetOrCoefficientNeighbours) {
  constexpr unsigned Width = 4;
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
  SymRef F = scaledOffset(Ctx, X, 6, 3);
  for (int64_t Scale : {-6, -5, 6})
    for (int64_t Offset : {-5, -4, -3}) {
      SymRef G = scaledOffset(Ctx, X, Scale, Offset);
      SymRef E = Ctx.mkAdd(
          {Ctx.mkAnd(F, G), Ctx.mkXor(X, Y), Ctx.mkAnd(F, Y), Ctx.mkOr(G, Y)});
      for (bool Products : {false, true}) {
        detail::WorkBudget Budget(MBAOptions::UnlimitedWork);
        auto A = detail::abstractToMBA(Ctx, E, Products, &Budget);
        ASSERT_TRUE(A);
        SymRef Restored = Ctx.substitute(A->Body, A->Hidden);
        std::vector<uint64_t> Values(Ctx.numVars(), 0);
        for (unsigned XV = 0; XV < 16; ++XV)
          for (unsigned YV = 0; YV < 16; ++YV) {
            Values[Ctx.varId(X)] = XV;
            Values[Ctx.varId(Y)] = YV;
            EXPECT_EQ(Ctx.evalU64(Restored, Values), Ctx.evalU64(E, Values));
          }
      }
    }
}

TEST(SymMBAComplement, RefusalIsAtomicAcrossArithmeticAndBitwiseRecovery) {
  for (unsigned Width : {8u, 64u, 256u}) {
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    SymRef F = scaledOffset(Ctx, X, 3, 5);
    SymRef G = scaledOffset(Ctx, X, -3, -6);
    SymRef E = Ctx.mkAdd({X, Ctx.mkAnd(F, G), Ctx.mkAnd(F, Y)});
    detail::WorkBudget Zero(0);
    auto Original = detail::abstractToMBA(Ctx, E, false, &Zero);
    detail::WorkBudget Unlimited(MBAOptions::UnlimitedWork);
    auto Full = detail::abstractToMBA(Ctx, E, false, &Unlimited);
    ASSERT_TRUE(Original);
    ASSERT_TRUE(Full);
    ASSERT_NE(Original->Body, Full->Body);
    const size_t Step = std::max(size_t(1), Unlimited.used() / 23);
    for (size_t Limit = 0; Limit < Unlimited.used(); Limit += Step) {
      detail::WorkBudget Tight(Limit);
      auto A = detail::abstractToMBA(Ctx, E, false, &Tight);
      ASSERT_TRUE(A);
      EXPECT_EQ(A->Body, Original->Body);
      EXPECT_EQ(A->Hidden, Original->Hidden);
      EXPECT_TRUE(Tight.exhausted());
    }
    detail::WorkBudget NoBytes(MBAOptions::UnlimitedWork);
    auto Declined = detail::abstractToMBA(Ctx, E, false, &NoBytes, 0);
    ASSERT_TRUE(Declined);
    EXPECT_EQ(Declined->Body, Original->Body);
    bool AcceptedStorage = false;
    for (size_t Bytes = 1; Bytes < 32768; Bytes += 127) {
      detail::WorkBudget Work(MBAOptions::UnlimitedWork);
      auto Limited = detail::abstractToMBA(Ctx, E, false, &Work, Bytes);
      ASSERT_TRUE(Limited);
      EXPECT_TRUE(Limited->Body == Original->Body ||
                  Limited->Body == Full->Body);
      EXPECT_EQ(Limited->Hidden, Original->Hidden);
      AcceptedStorage |= Limited->Body == Full->Body;
    }
    EXPECT_TRUE(AcceptedStorage);
    detail::WorkBudget Retry(MBAOptions::UnlimitedWork);
    auto Again = detail::abstractToMBA(Ctx, E, false, &Retry);
    ASSERT_TRUE(Again);
    EXPECT_EQ(Again->Body, Full->Body);
    EXPECT_EQ(Retry.used(), Unlimited.used());
  }
}

TEST(SymMBAComplement, DoesNotRecursivelyExpandRelatedInputTemplates) {
  constexpr unsigned Width = 3;
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
  SymRef Z = Ctx.mkVar("z", Width);
  SymRef P = scaledOffset(Ctx, Ctx.mkAdd(X, Y), 3, 5);
  SymRef Q =
      Ctx.mkAdd(scaledOffset(Ctx, X, -3, -6), scaledOffset(Ctx, Y, -3, 0));
  SymRef R = Ctx.mkAdd(scaledOffset(Ctx, X, 3, 5), scaledOffset(Ctx, Y, 3, 0));
  ASSERT_NE(P, R);
  SymRef E = Ctx.mkAdd({Ctx.mkAnd(P, Q), Ctx.mkOr(Q, R), Ctx.mkXor(P, R),
                        Ctx.mkAnd(P, Z), Ctx.mkOr(R, Z)});
  for (bool Products : {false, true}) {
    detail::WorkBudget Budget(MBAOptions::UnlimitedWork);
    auto A = detail::abstractToMBA(Ctx, E, Products, &Budget);
    ASSERT_TRUE(A);
    SymRef Restored = Ctx.substitute(A->Body, A->Hidden);
    std::vector<uint64_t> Values(Ctx.numVars(), 0);
    for (unsigned XV = 0; XV < 8; ++XV)
      for (unsigned YV = 0; YV < 8; ++YV)
        for (unsigned ZV = 0; ZV < 8; ++ZV) {
          Values[Ctx.varId(X)] = XV;
          Values[Ctx.varId(Y)] = YV;
          Values[Ctx.varId(Z)] = ZV;
          EXPECT_EQ(Ctx.evalU64(Restored, Values), Ctx.evalU64(E, Values));
        }
    detail::WorkBudget Again(MBAOptions::UnlimitedWork);
    auto Same = detail::abstractToMBA(Ctx, E, Products, &Again);
    ASSERT_TRUE(Same);
    EXPECT_EQ(A->Body, Same->Body);
    EXPECT_EQ(Budget.used(), Again.used());
  }
}

TEST(SymMBAComplement, IndexesManySharedInputsWithinLinearWork) {
  size_t Previous = 0;
  for (unsigned Count : {32u, 64u, 128u}) {
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", 64), Y = Ctx.mkVar("y", 64);
    llvm::SmallVector<SymRef, 32> Terms;
    for (unsigned I = 0; I < Count; ++I) {
      SymRef Mask = Ctx.mkVar("m" + std::to_string(I), 64);
      SymRef F = scaledOffset(Ctx, X, 2 * I + 2, I);
      SymRef G = scaledOffset(Ctx, X, -int64_t(2 * I + 2), -int64_t(I) - 1);
      Terms.push_back(Ctx.mkAnd(F, G));
      Terms.push_back(Ctx.mkAnd(F, Mask));
      Terms.push_back(Ctx.mkXor(G, Y));
    }
    detail::WorkBudget Budget(MBAOptions::UnlimitedWork);
    auto A = detail::abstractToMBA(Ctx, Ctx.mkAdd(Terms), false, &Budget);
    ASSERT_TRUE(A);
    if (Previous)
      EXPECT_LT(Budget.used(), 3 * Previous);
    Previous = Budget.used();
  }
}

} // namespace
