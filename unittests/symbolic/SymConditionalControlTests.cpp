//===- SymConditionalControlTests.cpp - Branch-scoped rewrites ------------===//

#include "gtest/gtest.h"

#include "neverd/solver/BitVectorSolver.h"
#include "neverd/symbolic/SymMBA.h"

using namespace neverd::symbolic;

namespace {

TEST(SymConditionalControls, RepeatedGuardsSpecializeOnlyTheirOwnArms) {
  for (unsigned Width : {8u, 32u, 64u, 128u, 256u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto P = C.mkVar("p", 1), Q = C.mkVar("q", 1);
    auto A = C.mkVar("a", Width), B = C.mkVar("b", Width);
    auto D = C.mkVar("d", Width);
    auto Inner = C.mkIte(P, A, B);
    auto Root = C.mkIte(P, Inner, D);
    auto Want = C.mkIte(P, A, D);
    EXPECT_NE(Root, Want);
    EXPECT_EQ(simplifyMBADeep(C, Root).Expr, Want);
    auto Other = C.mkIte(Q, Inner, D);
    auto Reduced = simplifyMBADeep(C, Other).Expr;
    neverd::solver::BitVectorSolver Proof(C);
    EXPECT_EQ(Proof.check(C.mkNe(Other, Reduced)),
              neverd::solver::SatResult::Unsat);
    EXPECT_EQ(Proof.check(C.mkNe(Other, C.mkIte(Q, A, D))),
              neverd::solver::SatResult::Sat);
    // Specializing one use must not change a separately observed inner guard.
    EXPECT_EQ(C.operand(Inner, 0), P);
    EXPECT_EQ(C.operand(Inner, 2), B);
  }
}

TEST(SymConditionalControls, ConstantTagsRecoverThePredicate) {
  SymContext C;
  auto P = C.mkVar("p", 1);
  for (unsigned Width : {8u, 64u, 128u, 256u}) {
    auto A = C.mkConst(Width, 3), B = C.mkConst(Width, 7);
    auto Tagged = C.mkIte(P, A, B);
    EXPECT_EQ(simplifyMBADeep(C, C.mkEq(Tagged, A)).Expr, P);
    EXPECT_EQ(simplifyMBADeep(C, C.mkEq(Tagged, B)).Expr, C.mkNot(P));
    EXPECT_EQ(simplifyMBADeep(C, C.mkEq(Tagged, C.mkConst(Width, 5))).Expr,
              C.mkZero(1));
  }
}

TEST(SymConditionalControls, PartialBudgetsMatchAnIndependentTruthTable) {
  SymContext C;
  auto P = C.mkVar("p", 1), Q = C.mkVar("q", 1);
  auto X = C.mkVar("x", 4), Y = C.mkVar("y", 4);
  auto Root =
      C.mkIte(P, C.mkIte(P, C.mkIte(Q, X, Y), Y), C.mkIte(P, X, C.mkAdd(X, Y)));
  for (unsigned Work : {0u, 1u, 3u, 16u, 64u, 10000u}) {
    MBAOptions Options;
    Options.MaxWork = Work;
    auto Result = simplifyMBADeep(C, Root, Options);
    EXPECT_LE(Result.Work, Work);
    if (!Work)
      EXPECT_EQ(Result.Expr, Root);
    for (unsigned A = 0; A != 16; ++A)
      for (unsigned B = 0; B != 16; ++B)
        for (unsigned Guard = 0; Guard != 4; ++Guard) {
          uint64_t Inputs[] = {Guard & 1u, Guard >> 1, A, B};
          unsigned Want = (Guard & 1) ? ((Guard & 2) ? A : B) : ((A + B) & 15);
          EXPECT_EQ(C.evalU64(Result.Expr, Inputs), Want);
        }
  }
}

TEST(SymConditionalControls, DataArmsAreNeverAssignedTheControlValue) {
  SymContext C;
  auto P = C.mkVar("p", 1), X = C.mkVar("x", 64);
  auto Root = C.mkIte(P, C.mkAdd(X, C.mkOne(64)), X);
  auto Result = simplifyMBADeep(C, Root).Expr;
  neverd::solver::BitVectorSolver Proof(C);
  EXPECT_EQ(Proof.check(C.mkNe(Root, Result)),
            neverd::solver::SatResult::Unsat);
  EXPECT_EQ(Proof.check(C.mkNe(Result, X)), neverd::solver::SatResult::Sat);
}

} // namespace
