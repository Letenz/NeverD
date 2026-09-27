//===- SymMBABooleanResidualTests.cpp - Weighted Boolean pairs ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/symbolic/SymMBA.h"

#include <array>
#include <string>
#include <utility>
#include <vector>

using namespace neverd::symbolic;

namespace {

SymRef scaled(SymContext &Ctx, const llvm::APInt &K, SymRef E) {
  return Ctx.mkMul(Ctx.mkConst(K), E);
}

SymRef expandedPair(SymContext &Ctx, SymRef X, SymRef Y, SymRef Z,
                    const llvm::APInt &A, const llvm::APInt &B) {
  const llvm::APInt TwiceB = B + B;
  return Ctx.mkAdd(
      {scaled(Ctx, B, Ctx.mkAdd({X, Y, Z})),
       scaled(Ctx, A - TwiceB,
              Ctx.mkAdd({Ctx.mkAnd(X, Y), Ctx.mkAnd(X, Z), Ctx.mkAnd(Y, Z)})),
       scaled(Ctx, TwiceB + TwiceB - A - A, Ctx.mkAnd({X, Y, Z}))});
}

SymRef compactPair(SymContext &Ctx, SymRef X, SymRef Y, SymRef Z,
                   const llvm::APInt &A, const llvm::APInt &B) {
  SymRef Majority = Ctx.mkOr(Ctx.mkAnd(X, Y), Ctx.mkAnd(Z, Ctx.mkOr(X, Y)));
  return Ctx.mkAdd(scaled(Ctx, A, Majority),
                   scaled(Ctx, B, Ctx.mkXor({X, Y, Z})));
}

TEST(SymMBABooleanResidual, RecoversTwoSelectorsWithoutAnAffineAtom) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 8), Y = Ctx.mkVar("y", 8);
  SymRef Z = Ctx.mkVar("z", 8);
  const llvm::APInt A(8, 2), B(8, 5);
  SymRef E = expandedPair(Ctx, X, Y, Z, A, B);
  SymRef Expected = compactPair(Ctx, X, Y, Z, A, B);
  MBAOptions Options;
  Options.VerifySamples = 0;
  auto Result = simplifyMBA(Ctx, E, Options);
  EXPECT_LT(Result.SizeAfter, Result.SizeBefore);
  EXPECT_LE(Result.SizeAfter, Ctx.readabilityCost(Expected))
      << Ctx.toString(Result.Expr);
  detail::WorkBudget Proof(MBAOptions::UnlimitedWork);
  EXPECT_TRUE(detail::proveLinearIdentity(Ctx, E, Result.Expr, 3, Proof));
  EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
}

TEST(SymMBABooleanResidual, HandlesOddEvenOffsetsAndWideCoefficients) {
  for (unsigned Width : {8u, 32u, 64u, 129u, 257u}) {
    SCOPED_TRACE(Width);
    for (auto Pair : {std::pair{2, 5}, {4, 9}, {-3, 8}, {3, -10}}) {
      SCOPED_TRACE(::testing::PrintToString(Pair));
      SymContext Ctx;
      SymRef X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
      SymRef Z = Ctx.mkVar("z", Width);
      llvm::APInt A = llvm::APInt(64, Pair.first, true).sextOrTrunc(Width);
      llvm::APInt B = llvm::APInt(64, Pair.second, true).sextOrTrunc(Width);
      if (Width > 64)
        A += llvm::APInt(Width, 1).shl(Width - 9);
      for (int Offset : {0, 1, -7}) {
        SCOPED_TRACE(Offset);
        SymRef K =
            Ctx.mkConst(llvm::APInt(64, Offset, true).sextOrTrunc(Width));
        SymRef E = Ctx.mkAdd(expandedPair(Ctx, X, Y, Z, A, B), K);
        SymRef Expected = Ctx.mkAdd(compactPair(Ctx, X, Y, Z, A, B), K);
        MBAOptions Options;
        Options.VerifySamples = 0;
        auto Result = simplifyMBA(Ctx, E, Options);
        EXPECT_LE(Result.SizeAfter, Ctx.readabilityCost(Expected))
            << Ctx.toString(Result.Expr);
        detail::WorkBudget Proof(MBAOptions::UnlimitedWork);
        EXPECT_TRUE(detail::proveLinearIdentity(Ctx, E, Result.Expr, 3, Proof));
      }
    }
  }
}

TEST(SymMBABooleanResidual, RestoresHiddenInputsAfterIndependentProof) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 64), Y = Ctx.mkVar("y", 64);
  SymRef Z = Ctx.mkVar("z", 64);
  SymRef Hidden = Ctx.mkUDiv(X, Y);
  SymRef E =
      expandedPair(Ctx, Hidden, Y, Z, llvm::APInt(64, 2), llvm::APInt(64, 5));
  SymRef Expected =
      compactPair(Ctx, Hidden, Y, Z, llvm::APInt(64, 2), llvm::APInt(64, 5));
  auto Result = simplifyMBA(Ctx, E);
  EXPECT_LE(Result.SizeAfter, Ctx.readabilityCost(Expected));
  EXPECT_LT(Result.SizeAfter, Result.SizeBefore);
  EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
}

TEST(SymMBABooleanResidual, RetainsExactBehaviorAtEverySmallBudget) {
  for (unsigned Width : {1u, 2u, 3u}) {
    SCOPED_TRACE(Width);
    for (bool Hidden : {false, true}) {
      for (bool Deep : {false, true}) {
        for (size_t Limit :
             {size_t(0), size_t(1), size_t(128), size_t(4096), size_t(65536),
              size_t(131072), size_t(262144), size_t(1) << 22}) {
          SCOPED_TRACE(Limit);
          SymContext Ctx;
          SymRef X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
          SymRef Z = Ctx.mkVar("z", Width);
          SymRef First = Hidden ? Ctx.mkUDiv(X, Y) : X;
          SymRef E =
              expandedPair(Ctx, First, Y, Z, llvm::APInt(64, 2).trunc(Width),
                           llvm::APInt(64, 5).trunc(Width));
          MBAOptions Options;
          Options.MaxWork = Limit;
          Options.VerifySamples = 0;
          auto Result = Deep ? simplifyMBADeep(Ctx, E, Options)
                             : simplifyMBA(Ctx, E, Options);
          EXPECT_LE(Result.Work, Limit);
          EXPECT_LE(Result.SizeAfter, Result.SizeBefore);
          SymEvalPlan Before(Ctx, E), After(Ctx, Result.Expr);
          std::vector<uint64_t> Assignment(Ctx.numVars(), 0);
          const unsigned Mask = (1u << Width) - 1;
          for (unsigned K = 0; K < (1u << (3 * Width)); ++K) {
            Assignment[Ctx.varId(X)] = K & Mask;
            Assignment[Ctx.varId(Y)] = (K >> Width) & Mask;
            Assignment[Ctx.varId(Z)] = (K >> (2 * Width)) & Mask;
            ASSERT_EQ(Before.evalU64(Assignment), After.evalU64(Assignment));
          }
        }
      }
    }
  }
}

TEST(SymMBABooleanResidual, RejectsNonrectangularAndRepeatedValueTables) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 8), Y = Ctx.mkVar("y", 8);
  const std::array<SymRef, 3> Atoms{X, Y, Ctx.mkVar("z", 8)};
  for (auto Values :
       {std::array{0, 2, 5, 8, 2, 5, 8, 0}, {0, 2, 2, 4, 2, 2, 4, 0}}) {
    std::vector<llvm::APInt> Weights;
    for (int V : Values)
      Weights.emplace_back(8, V);
    llvm::SmallVector<SymRef, 8> Forms;
    detail::WorkBudget Budget(4096);
    detail::booleanResidualCandidates(Ctx, Weights, Atoms, 64,
                                      detail::resolveLimits(MBAOptions{}),
                                      Budget, Forms);
    EXPECT_TRUE(Forms.empty());
    EXPECT_LT(Budget.used(), 128u);
  }
}

TEST(SymMBABooleanResidual, KeepsPreviouslyCoveredTwoInputRectangles) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 8), Y = Ctx.mkVar("y", 8);
  SymRef E = Ctx.mkAdd(
      {scaled(Ctx, llvm::APInt(8, 7), X), scaled(Ctx, llvm::APInt(8, 5), Y),
       scaled(Ctx, llvm::APInt(8, -10, true), Ctx.mkAnd(X, Y))});
  SymRef Expected = Ctx.mkAdd(scaled(Ctx, llvm::APInt(8, 2), X),
                              scaled(Ctx, llvm::APInt(8, 5), Ctx.mkXor(X, Y)));
  auto Result = simplifyMBA(Ctx, E);
  EXPECT_LE(Result.SizeAfter, Ctx.readabilityCost(Expected));
  detail::WorkBudget Proof(MBAOptions::UnlimitedWork);
  EXPECT_TRUE(detail::proveLinearIdentity(Ctx, E, Result.Expr, 2, Proof));
}

TEST(SymMBABooleanResidual, HonorsSynthesisArityAndWideScratchLimits) {
  for (unsigned Width : {129u, 4096u}) {
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    SymRef Z = Ctx.mkVar("z", Width);
    SymRef E = expandedPair(Ctx, X, Y, Z, llvm::APInt(Width, 2),
                            llvm::APInt(Width, 5));
    MBAOptions Tight;
    Tight.MaxTableBytes = 256;
    Tight.VerifySamples = 0;
    auto Limited = simplifyMBA(Ctx, E, Tight);
    auto Full = simplifyMBA(Ctx, E);
    EXPECT_LT(Full.SizeAfter, Limited.SizeAfter);
    EXPECT_LE(Limited.SizeAfter, Limited.SizeBefore);
    detail::WorkBudget Proof(MBAOptions::UnlimitedWork);
    EXPECT_TRUE(detail::proveLinearIdentity(Ctx, E, Limited.Expr, 3, Proof));
  }
  SymContext Ctx;
  llvm::SmallVector<SymRef, 4> Atoms;
  for (unsigned I = 0; I != 4; ++I)
    Atoms.push_back(Ctx.mkVar("x" + std::to_string(I), 8));
  std::vector<llvm::APInt> Weights(16, llvm::APInt(8, 0));
  llvm::SmallVector<SymRef, 8> Forms;
  detail::WorkBudget Budget(4096);
  detail::booleanResidualCandidates(Ctx, Weights, Atoms, 64,
                                    detail::resolveLimits(MBAOptions{}), Budget,
                                    Forms);
  EXPECT_TRUE(Forms.empty());
  EXPECT_EQ(Budget.used(), 0u);
}

} // namespace
