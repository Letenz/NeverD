//===- SymMBARewriteCacheTests.cpp - Exact cross-query reuse --------------===//

#include "gtest/gtest.h"

#include "neverd/solver/BitVectorSolver.h"
#include "neverd/symbolic/SymMBA.h"

#include <stdexcept>

using namespace neverd::symbolic;

namespace {

SymRef obfuscateSum(SymContext &C, SymRef X, SymRef Y) {
  return C.mkAdd(C.mkXor(X, Y),
                 C.mkMul(C.mkConst(C.width(X), 2), C.mkAnd(X, Y)));
}

TEST(SymMBARewriteCache, WarmChildReducesWorkUnderAnOpaqueParent) {
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8), Z = C.mkVar("z", 8);
  auto Sum = obfuscateSum(C, X, Y);
  MBARewriteCache Cache(C);
  MBAOptions Options;
  Options.VerifySamples = 0;
  auto Warm = simplifyMBADeep(C, Sum, Options, Cache);
  ASSERT_EQ(Warm.Expr, C.mkAdd(X, Y));
  ASSERT_GT(Cache.size(), 0u);
  auto Root = C.mkUDiv(Sum, Z);
  auto Cold = simplifyMBADeep(C, Root, Options);
  auto Hot = simplifyMBADeep(C, Root, Options, Cache);
  EXPECT_EQ(Cold.Expr, Hot.Expr);
  EXPECT_LT(Hot.Work, Cold.Work);
  EXPECT_GT(Cache.reusedRoots(), 0u);
  RecordProperty("uncached_work", Cold.Work);
  RecordProperty("cached_work", Hot.Work);
  for (unsigned A = 0; A != 256; ++A)
    for (unsigned B = 0; B != 256; ++B) {
      uint64_t Inputs[] = {A, B, 3};
      EXPECT_EQ(C.evalU64(Hot.Expr, Inputs), ((A + B) & 255) / 3);
    }
}

TEST(SymMBARewriteCache, ReusingAnExactRootHasOneWorkUnit) {
  SymContext C;
  auto X = C.mkVar("x", 64), Y = C.mkVar("y", 64);
  auto Root = obfuscateSum(C, X, Y);
  MBARewriteCache Cache(C);
  auto Warm = simplifyMBADeep(C, Root, {}, Cache);
  ASSERT_TRUE(Warm.Changed);
  MBAOptions Tight;
  Tight.MaxWork = 1;
  auto Hot = simplifyMBADeep(C, Root, Tight, Cache);
  EXPECT_EQ(Hot.Expr, Warm.Expr);
  EXPECT_EQ(Hot.Work, 1u);
  EXPECT_EQ(Hot.Evidence, MBAEvidence::Derivation);
}

TEST(SymMBARewriteCache, ZeroWorkNeverConsumesAnExistingRewrite) {
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  auto Root = obfuscateSum(C, X, Y);
  MBARewriteCache Cache(C);
  ASSERT_TRUE(simplifyMBADeep(C, Root, {}, Cache).Changed);
  MBAOptions Zero;
  Zero.MaxWork = 0;
  auto Result = simplifyMBADeep(C, Root, Zero, Cache);
  EXPECT_EQ(Result.Expr, Root);
  EXPECT_EQ(Result.Work, 0u);
  EXPECT_EQ(Cache.reusedRoots(), 0u);
}

TEST(SymMBARewriteCache, CapacityBoundsEntriesAndDisabledCacheRetainsNothing) {
  SymContext C;
  MBARewriteCache Cache(C, 1), Disabled(C, 0);
  auto X = C.mkVar("x", 32);
  for (unsigned I = 0; I != 5; ++I) {
    auto Y = C.mkVar("y" + std::to_string(I), 32);
    auto Root = obfuscateSum(C, X, Y);
    EXPECT_EQ(simplifyMBADeep(C, Root, {}, Cache).Expr, C.mkAdd(X, Y));
    EXPECT_EQ(simplifyMBADeep(C, Root, {}, Disabled).Expr, C.mkAdd(X, Y));
    EXPECT_LE(Cache.size(), 1u);
    EXPECT_EQ(Disabled.size(), 0u);
  }
}

TEST(SymMBARewriteCache, ForeignContextsFailBeforePublishingAnyFact) {
  SymContext A, B;
  MBARewriteCache Cache(A);
  auto X = B.mkVar("x", 8), Y = B.mkVar("y", 8);
  EXPECT_THROW(simplifyMBADeep(B, obfuscateSum(B, X, Y), {}, Cache),
               std::invalid_argument);
  EXPECT_EQ(Cache.size(), 0u);
}

TEST(SymMBARewriteCache, IdentitiesAndWideCarriesRemainContextLocal) {
  SymContext C;
  auto X = C.mkVar("x", 128), Y = C.mkVar("y", 128);
  auto A = C.mkVar("a", 128), B = C.mkVar("b", 128);
  MBARewriteCache Cache(C);
  ASSERT_TRUE(simplifyMBADeep(C, obfuscateSum(C, X, Y), {}, Cache).Changed);
  auto Root = C.mkLShr(obfuscateSum(C, A, B), C.mkConst(128, 64));
  auto Result = simplifyMBADeep(C, Root, {}, Cache);
  neverd::solver::BitVectorSolver Proof(C);
  EXPECT_EQ(Proof.check(C.mkNe(Root, Result.Expr)),
            neverd::solver::SatResult::Unsat);
  auto Other = C.mkLShr(C.mkAdd(X, Y), C.mkConst(128, 64));
  EXPECT_EQ(Proof.check(C.mkNe(Result.Expr, Other)),
            neverd::solver::SatResult::Sat);
}

} // namespace
