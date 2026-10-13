//===- FreshChoiceProjectionTests.cpp - Incremental zero projections -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks fresh-leaf admission, exact substitution and total traversal work.
//===----------------------------------------------------------------------===//

#include "../../lib/analysis/core/FreshChoiceProjection.h"
#include "gtest/gtest.h"

#include <array>
#include <bit>
#include <type_traits>

using namespace neverd::analysis::detail;
using namespace neverd::symbolic;

namespace {

void sameContexts(const SymContext &A, const SymContext &B) {
  ASSERT_EQ(A.numNodes(), B.numNodes());
  ASSERT_EQ(A.numVars(), B.numVars());
  for (uint32_t I = 0; I != A.numNodes(); ++I) {
    const SymRef R(I);
    const auto &X = A.node(R), &Y = B.node(R);
    EXPECT_EQ(X.Op, Y.Op);
    EXPECT_EQ(X.Width, Y.Width);
    EXPECT_EQ(X.FirstOperand, Y.FirstOperand);
    EXPECT_EQ(X.NumOperands, Y.NumOperands);
    EXPECT_EQ(X.Aux, Y.Aux);
    EXPECT_EQ(A.operands(R), B.operands(R));
    if (A.isConst(R))
      EXPECT_EQ(A.constValue(R), B.constValue(R));
  }
  for (uint32_t I = 0; I != A.numVars(); ++I) {
    const auto &X = A.varInfo(I), &Y = B.varInfo(I);
    EXPECT_EQ(X.Name, Y.Name);
    EXPECT_EQ(X.Width, Y.Width);
    EXPECT_EQ(X.Fresh, Y.Fresh);
    EXPECT_EQ(X.InputOrigin, Y.InputOrigin);
  }
}

struct Paired {
  SymContext Original, Cached;
  FreshChoiceProjection Projection;
  std::unordered_map<uint32_t, SymRef> Map;

  explicit Paired(uint64_t Limit) : Projection(Cached, Limit) {}

  template <typename F> SymRef make(F Build) {
    const auto A = Build(Original), B = Build(Cached);
    EXPECT_EQ(A, B);
    return A;
  }

  SymRef fresh(unsigned Width) {
    return make([&](SymContext &C) { return C.mkFreshVar(Width, "choice"); });
  }

  void add(SymRef R) {
    Map.emplace(R.index(), Original.mkZero(Original.width(R)));
    ASSERT_TRUE(Projection.addFreshZero(Cached, R));
    sameContexts(Original, Cached);
  }

  SymRef project(SymRef R) {
    // Neither arena has been primed by the other one's substitution.
    const auto Expected = Original.substitute(R, Map);
    const auto Actual = Projection.project(Cached, R);
    EXPECT_EQ(Expected, Actual);
    sameContexts(Original, Cached);
    return Actual;
  }
};

TEST(FreshChoiceProjection, EmptyMapKeepsValuesAndClosesTheOldPrefix) {
  SymContext C;
  FreshChoiceProjection P(C, 4096);
  const auto Old = C.mkFreshVar(8);
  const auto Root = C.mkAdd(Old, C.mkConst(8, 17));
  const auto Nodes = C.numNodes();
  EXPECT_EQ(P.project(C, Root), Root);
  EXPECT_EQ(C.numNodes(), Nodes);
  EXPECT_EQ(P.allocatedNodes(), 0u);
  EXPECT_EQ(P.statistics().WorkItems, 0u);
  EXPECT_FALSE(P.addFreshZero(C, Old));
  const auto New = C.mkFreshVar(8);
  ASSERT_TRUE(P.addFreshZero(C, New));
  EXPECT_EQ(P.project(C, C.mkXor(Root, New)), Root);
}

TEST(FreshChoiceProjection, HandWrittenDagCountsAllTraversalWork) {
  SymContext C;
  FreshChoiceProjection P(C, 4096);
  const auto X = C.mkVar("x", 8), U = C.mkFreshVar(8), V = C.mkFreshVar(8);
  ASSERT_TRUE(P.addFreshZero(C, U));
  const auto Root = C.mkXor(X, U);
  EXPECT_EQ(P.project(C, Root), X);
  EXPECT_EQ(P.statistics().Queries, 1u);
  // Root and x have an enter/exit each; the replaced leaf has one enter.
  EXPECT_EQ(P.statistics().WorkItems, 5u);
  EXPECT_EQ(P.statistics().OperandScans, 4u);
  EXPECT_EQ(P.statistics().Rebuilds, 1u);
  EXPECT_EQ(P.statistics().CacheHits, 0u);
  EXPECT_EQ(P.project(C, Root), X);
  EXPECT_EQ(P.statistics().WorkItems, 5u);
  EXPECT_EQ(P.statistics().OperandScans, 4u);
  EXPECT_EQ(P.statistics().CacheHits, 1u);
  const auto Parent = C.mkAdd(Root, V);
  const auto Image = P.project(C, Parent);
  EXPECT_EQ(Image, C.mkAdd(X, V));
  // Parent and v add four pops; the two operands add two scans each.
  EXPECT_EQ(P.statistics().WorkItems, 9u);
  EXPECT_EQ(P.statistics().OperandScans, 8u);
  EXPECT_EQ(P.statistics().Rebuilds, 2u);
  EXPECT_EQ(P.statistics().CacheHits, 2u);
  EXPECT_EQ(P.statistics().Fallbacks, 0u);
  EXPECT_FALSE(P.addFreshZero(C, V));
}

TEST(FreshChoiceProjection, IndependentArenasRetainNodeOrderAcrossNewChoices) {
  for (unsigned Width : {1u, 8u, 64u, 65u, 128u}) {
    Paired P(32768);
    for (unsigned I = 0; I != 12; ++I)
      P.make([&](SymContext &C) { return C.mkConst(64, I + 7); });
    const auto X =
        P.make([&](SymContext &C) { return C.mkVar("input", Width); });
    SymRef Root = X;
    P.project(Root);
    for (unsigned I = 0; I != 24; ++I) {
      const auto Previous = Root;
      const auto U = P.fresh(Width);
      // As in the checker, a distinct right choice precedes the zero literal.
      const auto Right = P.fresh(Width);
      ASSERT_NE(U.index(), P.Cached.varId(U));
      P.add(U);
      Root = P.make([&](SymContext &C) {
        const auto Mixed = C.mkAdd(C.mkXor(Root, U), Right);
        return C.mkIte(C.mkEq(U, X), Mixed, C.mkNot(Mixed));
      });
      P.project(Root);
      P.project(Previous);
      P.project(Root);
      // A later fresh mapping must not erase an earlier right-hand choice.
      EXPECT_EQ(P.project(Right), Right);
    }
    EXPECT_EQ(P.Projection.statistics().Fallbacks, 0u);
  }
}

TEST(FreshChoiceProjection,
     ByteInputsAndUnmappedChoicesMatchAnIndependentOracle) {
  Paired P(4096);
  const auto X = P.make([](SymContext &C) { return C.mkVar("x", 8); });
  const auto Y = P.make([](SymContext &C) { return C.mkVar("y", 8); });
  const auto U = P.fresh(8), Right = P.fresh(8), Guard = P.fresh(1);
  P.add(U);
  P.add(Guard);
  const auto Root = P.make([&](SymContext &C) {
    const auto Word = C.mkConcat({C.mkXor(X, U), C.mkAdd(Y, U)});
    const auto Low = C.mkExtract(Word, 0, 8);
    const auto High = C.mkExtract(Word, 8, 8);
    const auto Sum =
        C.mkAdd(C.mkXor(High, Right), C.mkRol(Low, C.mkConst(8, 3)));
    return C.mkIte(Guard, C.mkNot(Sum), Sum);
  });
  const auto Image = P.project(Root);
  std::vector<llvm::APInt> Values;
  for (unsigned I = 0; I != P.Cached.numVars(); ++I)
    Values.emplace_back(P.Cached.varInfo(I).Width, 0);
  for (unsigned A = 0; A != 256; ++A)
    for (unsigned B : {0u, 1u, 127u, 128u, 255u})
      for (unsigned R : {0u, 0x55u, 0xffu}) {
        Values[P.Cached.varId(X)] = llvm::APInt(8, A);
        Values[P.Cached.varId(Y)] = llvm::APInt(8, B);
        Values[P.Cached.varId(Right)] = llvm::APInt(8, R);
        // Removed variables can take nonzero values without changing Image.
        Values[P.Cached.varId(U)] = llvm::APInt(8, A ^ 0xa5);
        Values[P.Cached.varId(Guard)] = llvm::APInt(1, 1);
        const unsigned Rotated = ((B << 3) | (B >> 5)) & 255;
        EXPECT_EQ(P.Cached.eval(Image, Values).getZExtValue(),
                  ((A ^ R) + Rotated) & 255);
      }
}

TEST(FreshChoiceProjection,
     BadRegistrationsAndForeignContextsCannotChangeResults) {
  SymContext A, B;
  FreshChoiceProjection P(A, 4096);
  const auto U = A.mkFreshVar(8), Foreign = B.mkFreshVar(8);
  ASSERT_EQ(U.index(), Foreign.index());
  EXPECT_FALSE(P.addFreshZero(B, Foreign));
  EXPECT_FALSE(P.project(B, Foreign));
  const auto Named = A.mkVar("ordinary", 8);
  const auto Input =
      A.mkInputVar("entry", 8, {SymInputKind::Register, 9, 1, 0});
  const auto Composite = A.mkNot(U);
  for (auto Invalid : {SymRef{}, SymRef{uint32_t(A.numNodes() + 5)}, Named,
                       Input, Composite, A.mkConst(8, 7)})
    EXPECT_FALSE(P.addFreshZero(A, Invalid));
  const auto Nodes = A.numNodes();
  EXPECT_EQ(P.allocatedNodes(), 0u);
  EXPECT_FALSE(P.project(A, SymRef{}));
  EXPECT_FALSE(P.project(A, SymRef{uint32_t(Nodes + 5)}));
  ASSERT_TRUE(P.addFreshZero(A, U));
  EXPECT_FALSE(P.addFreshZero(A, U));
  const auto Root = A.mkXor(U, Named);
  EXPECT_EQ(P.project(A, Root), Named);
  const auto Old = A.mkFreshVar(8);
  P.project(A, Old);
  EXPECT_FALSE(P.addFreshZero(A, Old));
  EXPECT_EQ(P.project(A, Root), Named);
  EXPECT_FALSE(P.project(B, Foreign));
  static_assert(!std::is_copy_constructible_v<FreshChoiceProjection>);
  static_assert(!std::is_move_constructible_v<FreshChoiceProjection>);
}

TEST(FreshChoiceProjection,
     ZeroAndBoundedCapacityFallbacksKeepExactNodeSequences) {
  for (uint64_t Limit : {0u, 1u, 5u, 8u, 17u, 128u}) {
    Paired P(Limit);
    const auto X = P.make([](SymContext &C) { return C.mkVar("input", 8); });
    SymRef Root = X;
    unsigned Fallbacks = 0;
    for (unsigned I = 0; I != 32; ++I) {
      const auto U = P.fresh(8);
      P.add(U);
      Root = P.make([&](SymContext &C) {
        return C.mkRol(C.mkXor(Root, U), C.mkConst(8, 1));
      });
      P.project(Root);
      Fallbacks += Root.index() >= Limit;
      EXPECT_LE(P.Projection.allocatedNodes(), Limit);
    }
    EXPECT_EQ(P.Projection.statistics().Fallbacks, Fallbacks);
    if (!Limit)
      EXPECT_EQ(P.Projection.allocatedNodes(), 0u);
  }
}

TEST(FreshChoiceProjection, ExactCapacityIncludesTheRootOnlyWhenItsIndexFits) {
  for (bool Fits : {false, true}) {
    SymContext C;
    const auto X = C.mkVar("x", 8), U = C.mkFreshVar(8);
    const auto Root = C.mkXor(X, U);
    FreshChoiceProjection P(C, uint64_t(Root.index()) + Fits);
    ASSERT_TRUE(P.addFreshZero(C, U));
    EXPECT_EQ(P.project(C, Root), X);
    EXPECT_EQ(P.statistics().Fallbacks, Fits ? 0u : 1u);
    EXPECT_LE(P.allocatedNodes(), uint64_t(Root.index()) + Fits);
  }
}

TEST(FreshChoiceProjection, SharedIncrementalChainsHaveLinearTotalTraversal) {
  constexpr unsigned Steps = 1024;
  SymContext C;
  FreshChoiceProjection P(C, 32768);
  const auto X = C.mkVar("x", 8), Shift = C.mkConst(8, 1);
  SymRef Root = X, Image;
  for (unsigned I = 0; I != Steps; ++I) {
    const auto Previous = Root, U = C.mkFreshVar(8);
    ASSERT_TRUE(P.addFreshZero(C, U));
    Root = C.mkRol(C.mkXor(Root, U), Shift);
    Image = P.project(C, Root);
    const auto Work = P.statistics().WorkItems;
    const auto Edges = P.statistics().OperandScans;
    const auto Hits = P.statistics().CacheHits;
    EXPECT_EQ(P.project(C, Root), Image);
    EXPECT_TRUE(P.project(C, Previous));
    EXPECT_EQ(P.statistics().WorkItems, Work);
    EXPECT_EQ(P.statistics().OperandScans, Edges);
    EXPECT_EQ(P.statistics().CacheHits, Hits + 2);
  }
  // Each new pair of binary operators contributes at most eight edge scans
  // and five stack pops. Two ordinary leaves add four initial pops.
  EXPECT_LE(P.statistics().WorkItems, 5u * Steps + 4);
  EXPECT_LE(P.statistics().OperandScans, 8u * Steps);
  EXPECT_LE(P.statistics().Rebuilds, 2u * Steps);
  EXPECT_EQ(P.statistics().Queries, 3u * Steps);
  EXPECT_EQ(P.statistics().Fallbacks, 0u);
  EXPECT_LE(P.allocatedNodes(), 32768u);
  std::vector<llvm::APInt> Values(C.numVars(), llvm::APInt(8, 0x5a));
  for (unsigned V = 0; V != 256; ++V) {
    Values[C.varId(X)] = llvm::APInt(8, V);
    EXPECT_EQ(C.eval(Image, Values).getZExtValue(),
              std::rotl(uint8_t(V), int(Steps % 8)));
  }
}

} // namespace
