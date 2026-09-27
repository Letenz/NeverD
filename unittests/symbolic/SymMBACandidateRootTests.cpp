//===- SymMBACandidateRootTests.cpp - Final candidate root readings -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymMBA.h"

#include <vector>

using namespace neverd::symbolic;

namespace {

struct RootCase {
  SymRef Input;
  SymRef Expected;
  SymRef X;
  SymRef Z;
};

RootCase productCarry(SymContext &Ctx, unsigned Width, unsigned Scale) {
  SymRef X = Ctx.mkVar("x", Width), Z = Ctx.mkVar("z", Width);
  SymRef Other = Ctx.mkMul(Ctx.mkConst(Width, Scale), X);
  SymRef Product = Ctx.mkAdd(Ctx.mkMul(Ctx.mkAnd(X, Other), Ctx.mkOr(X, Other)),
                             Ctx.mkMul(Ctx.mkAnd(X, Ctx.mkNot(Other)),
                                       Ctx.mkAnd(Ctx.mkNot(X), Other)));
  SymRef Mask = Ctx.mkAnd(X, Z);
  SymRef Target = Ctx.mkMul(X, Other);
  SymRef Input =
      Ctx.mkAdd({Ctx.mkConst(llvm::APInt::getAllOnes(Width) - 1),
                 Ctx.mkNeg(Mask), Product,
                 Ctx.mkMul(Ctx.mkConst(Width, 2),
                           Ctx.mkNeg(Ctx.mkOr(Target, Ctx.mkNot(Mask))))});
  return {Input, Ctx.mkXor(Mask, Target), X, Z};
}

SymRef bitwiseProduct(SymContext &Ctx, SymRef A, SymRef B) {
  return Ctx.mkAdd(
      Ctx.mkMul(Ctx.mkAnd(A, B), Ctx.mkOr(A, B)),
      Ctx.mkMul(Ctx.mkAnd(A, Ctx.mkNot(B)), Ctx.mkAnd(Ctx.mkNot(A), B)));
}

TEST(SymMBACandidateRoot, ReadsOriginalProductsAfterComplementImprovements) {
  for (unsigned Width : {3u, 8u, 64u, 128u}) {
    for (unsigned Scale : {1u, 2u, 3u}) {
      for (bool Repeated : {false, true}) {
        SCOPED_TRACE(Width);
        SCOPED_TRACE(Scale);
        SCOPED_TRACE(Repeated);
        SymContext Ctx;
        SymRef X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
        SymRef Product =
            Ctx.mkMul({Ctx.mkConst(Width, Scale), X, Repeated ? X : Y});
        SymRef Other = Ctx.mkAdd(Ctx.mkConst(Width, 1), Ctx.mkNot(Product));
        SymRef Input =
            Ctx.mkNot(Ctx.mkAdd(Ctx.mkConst(llvm::APInt::getAllOnes(Width)),
                                bitwiseProduct(Ctx, X, Other)));
        SymRef Expected = Ctx.mkMul(X, Product);
        MBAOptions Options;
        Options.VerifySamples = 0;
        auto Result = simplifyMBADeep(Ctx, Input, Options);
        EXPECT_LE(Ctx.readability(Result.Expr), Ctx.readability(Expected))
            << Ctx.toString(Result.Expr);
        EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
        EXPECT_LT(Result.Work, size_t(1) << 20);
        if (Width == 3) {
          std::vector<uint64_t> Values(Ctx.numVars(), 0);
          for (unsigned XV = 0; XV < 8; ++XV)
            for (unsigned YV = 0; YV < 8; ++YV) {
              Values[Ctx.varId(X)] = XV;
              Values[Ctx.varId(Y)] = YV;
              EXPECT_EQ(Ctx.evalU64(Input, Values),
                        Ctx.evalU64(Result.Expr, Values));
            }
        }
      }
    }
  }
}

TEST(SymMBACandidateRoot, ComparesCoefficientFactorsBeforeDiscardingBases) {
  for (unsigned Width : {8u, 32u, 64u, 128u}) {
    for (unsigned Scale : {2u, 3u, 5u}) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Scale);
      SymContext Ctx;
      SymRef X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
      SymRef Input =
          Ctx.mkAdd({Ctx.mkMul(Ctx.mkConst(Width, Scale), Ctx.mkXor(X, Y)),
                     Ctx.mkMul(Ctx.mkConst(Width, 2 * Scale + 1),
                               Ctx.mkNot(Ctx.mkOr(X, Y))),
                     bitwiseProduct(Ctx, X, Y), Ctx.mkNeg(Ctx.mkMul(X, Y))});
      SymRef Expected = Ctx.mkAdd(
          {Ctx.mkNeg(Ctx.mkConst(Width, 2 * Scale + 1)), Ctx.mkAnd(X, Y),
           Ctx.mkNeg(
               Ctx.mkMul(Ctx.mkConst(Width, Scale + 1), Ctx.mkAdd(X, Y)))});
      MBAOptions Options;
      Options.VerifySamples = 0;
      for (bool Deep : {false, true}) {
        auto Result = Deep ? simplifyMBADeep(Ctx, Input, Options)
                           : simplifyMBA(Ctx, Input, Options);
        EXPECT_LE(Ctx.readability(Result.Expr), Ctx.readability(Expected))
            << Ctx.toString(Result.Expr);
        EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
      }
    }
  }
}

TEST(SymMBACandidateRoot, MeasuresAnExposedAddRootAfterProductRecovery) {
  for (unsigned Width : {3u, 8u, 64u, 128u}) {
    for (unsigned Scale : {2u, 3u, 6u}) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Scale);
      SymContext Ctx;
      RootCase C = productCarry(Ctx, Width, Scale);
      MBAOptions Options;
      Options.VerifySamples = 0;
      auto Result = simplifyMBADeep(Ctx, C.Input, Options);
      EXPECT_LE(Result.SizeAfter, Ctx.readabilityCost(C.Expected))
          << Ctx.toString(Result.Expr);
      EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
      // Losing the shared complement makes this bounded recovery spend the
      // entire default allowance without reaching the product identity.
      EXPECT_LT(Result.Work, size_t(1) << 20);
      if (Width <= 3) {
        std::vector<uint64_t> Values(Ctx.numVars(), 0);
        for (unsigned X = 0; X < (1u << Width); ++X)
          for (unsigned Z = 0; Z < (1u << Width); ++Z) {
            Values[Ctx.varId(C.X)] = X;
            Values[Ctx.varId(C.Z)] = Z;
            EXPECT_EQ(Ctx.evalU64(C.Input, Values),
                      Ctx.evalU64(Result.Expr, Values));
          }
      }
    }
  }
}

TEST(SymMBACandidateRoot, ReusesARecoveredRootAcrossOpaqueConsumers) {
  SymContext Ctx;
  RootCase C = productCarry(Ctx, 8, 3);
  MBAOptions Options;
  Options.VerifySamples = 0;
  auto Single = simplifyMBADeep(Ctx, C.Input, Options);
  SymRef Three = Ctx.mkConst(8, 3), Five = Ctx.mkConst(8, 5);
  SymRef Shared =
      Ctx.mkAdd(Ctx.mkUDiv(C.Input, Three), Ctx.mkUDiv(C.Input, Five));
  SymRef Expected =
      Ctx.mkAdd(Ctx.mkUDiv(C.Expected, Three), Ctx.mkUDiv(C.Expected, Five));
  // Both opaque consumers share one expensive root. The extra allowance is
  // for their surrounding small region, not another copy of that search.
  Options.MaxWork = Single.Work + 4096;
  auto Result = simplifyMBADeep(Ctx, Shared, Options);
  EXPECT_EQ(Result.Expr, Expected) << Ctx.toString(Result.Expr);
  EXPECT_LE(Result.Work, Options.MaxWork);
  EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
}

TEST(SymMBACandidateRoot, KeepsTheSharedWorkAndStorageLimits) {
  for (unsigned Width : {1u, 2u, 3u}) {
    for (size_t Storage : {size_t(0), size_t(1024), size_t(1) << 28}) {
      for (size_t Work :
           {size_t(0), size_t(1), size_t(128), size_t(4096), size_t(1) << 20}) {
        SymContext Ctx;
        RootCase C = productCarry(Ctx, Width, 3);
        MBAOptions Options;
        Options.VerifySamples = 0;
        Options.MaxWork = Work;
        Options.MaxTableBytes = Storage;
        auto Result = simplifyMBADeep(Ctx, C.Input, Options);
        EXPECT_LE(Result.Work, Work);
        EXPECT_LE(Result.SizeAfter, Result.SizeBefore);
        std::vector<uint64_t> Values(Ctx.numVars(), 0);
        for (unsigned X = 0; X < (1u << Width); ++X)
          for (unsigned Z = 0; Z < (1u << Width); ++Z) {
            Values[Ctx.varId(C.X)] = X;
            Values[Ctx.varId(C.Z)] = Z;
            EXPECT_EQ(Ctx.evalU64(C.Input, Values),
                      Ctx.evalU64(Result.Expr, Values));
          }
      }
    }
  }
}

} // namespace
