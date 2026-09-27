//===- Z3SolverTests.cpp - Optional backend session contracts ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/solver/Z3Solver.h"

using namespace neverd::solver;
using namespace neverd::symbolic;

TEST(Z3Solver, DisabledBackendDoesNotSubstituteAnotherProver) {
  if (Z3Solver::available())
    GTEST_SKIP() << "This contract applies to builds without Z3";
  SymContext Ctx;
  const SymRef X = Ctx.mkVar("x", 8);
  Z3Solver Solver(Ctx);
  EXPECT_FALSE(Solver.ok());
  EXPECT_FALSE(Solver.assertTrue(Ctx.mkTrue()));
  EXPECT_EQ(Solver.check(), SatResult::Unknown);
  EXPECT_FALSE(Solver.reasonUnknown().empty());
  EXPECT_TRUE(Solver.model().empty());
  EXPECT_TRUE(Solver.dumpSMT2().empty());
  EXPECT_EQ(z3CheckEqual(Ctx, X, X), EquivResult::Unknown);
}

TEST(Z3Solver, MalformedQueriesRemainInvalidInEitherBuild) {
  SymContext Ctx;
  const SymRef X = Ctx.mkVar("x", 8);
  const SymRef Wide = Ctx.mkVar("x", 16);
  EXPECT_EQ(z3CheckSat(Ctx, SymRef()), SatResult::Invalid);
  EXPECT_EQ(z3CheckEqual(Ctx, X, Wide), EquivResult::Invalid);
  EXPECT_EQ(z3CheckEqual(Ctx, SymRef(), SymRef()), EquivResult::Invalid);
  Z3Solver Solver(Ctx);
  EXPECT_EQ(Solver.checkDistinct(X, Wide), SatResult::Invalid);
  EXPECT_EQ(Solver.check(), SatResult::Invalid);
  EXPECT_TRUE(Solver.model().empty());
}

TEST(Z3Solver, DistinctQueriesReuseTheSessionWithoutAddingAssertions) {
  if (!Z3Solver::available())
    GTEST_SKIP() << "Z3 backend disabled";
  SymContext Ctx;
  const SymRef X = Ctx.mkVar("x", 17);
  const SymRef Y = Ctx.mkVar("y", 17);
  Z3Solver Solver(Ctx);
  ASSERT_TRUE(Solver.assertEqual(X, Ctx.mkConst(17, 91)));
  EXPECT_EQ(Solver.checkDistinct(X, Ctx.mkConst(17, 91)), SatResult::Unsat);
  EXPECT_EQ(Solver.checkDistinct(X, Y), SatResult::Sat);
  ASSERT_TRUE(Solver.model().value(Ctx, X));
  ASSERT_TRUE(Solver.model().value(Ctx, Y));
  EXPECT_EQ(Solver.model().value(Ctx, X)->getZExtValue(), 91u);
  EXPECT_NE(Solver.model().value(Ctx, Y)->getZExtValue(), 91u);
  EXPECT_EQ(Solver.checkDistinct(X, X), SatResult::Unsat);
  EXPECT_TRUE(Solver.model().empty());
  EXPECT_EQ(Solver.check(), SatResult::Sat);
}

TEST(Z3Solver, AssumptionsHaveTemporaryModelsAndFailedCores) {
  if (!Z3Solver::available())
    GTEST_SKIP() << "Z3 backend disabled";
  SymContext Ctx;
  const SymRef X = Ctx.mkVar("x", 8);
  const SymRef One = Ctx.mkEq(X, Ctx.mkOne(8));
  const SymRef Two = Ctx.mkEq(X, Ctx.mkConst(8, 2));
  Z3Solver Solver(Ctx);
  EXPECT_EQ(Solver.check({One}), SatResult::Sat);
  ASSERT_TRUE(Solver.model().value(Ctx, X));
  EXPECT_EQ(Solver.model().value(Ctx, X)->getZExtValue(), 1u);
  EXPECT_EQ(Solver.check({Two}), SatResult::Sat);
  ASSERT_TRUE(Solver.model().value(Ctx, X));
  EXPECT_EQ(Solver.model().value(Ctx, X)->getZExtValue(), 2u);
  EXPECT_EQ(Solver.check({One, Two}), SatResult::Unsat);
  EXPECT_EQ(Solver.failedAssumptions().size(), 2u);
  EXPECT_TRUE(Solver.model().empty());
  EXPECT_EQ(Solver.check(), SatResult::Sat);
  EXPECT_TRUE(Solver.failedAssumptions().empty());
}

TEST(Z3Solver, DumpIncludesTheLastQueryAndDiscardsStaleAssumptions) {
  if (!Z3Solver::available())
    GTEST_SKIP() << "Z3 backend disabled";
  SymContext Ctx;
  const SymRef X = Ctx.mkVar("x", 8);
  Z3Solver Solver(Ctx);
  ASSERT_TRUE(Solver.assertTrue(X));
  const std::string Before = Solver.dumpSMT2();
  EXPECT_NE(Before.find("check-sat"), std::string::npos);
  ASSERT_EQ(Solver.checkDistinct(X, Ctx.mkConst(8, 57)), SatResult::Sat);
  const std::string Query = Solver.dumpSMT2();
  EXPECT_NE(Query, Before);
  EXPECT_EQ(Solver.check(), SatResult::Sat);
  EXPECT_EQ(Solver.dumpSMT2(), Before);
}

TEST(Z3Solver, ResourceExhaustionCannotBecomeAProof) {
  if (!Z3Solver::available())
    GTEST_SKIP() << "Z3 backend disabled";
  SymContext Ctx;
  const SymRef X = Ctx.mkVar("x", 64);
  const SymRef Y = Ctx.mkVar("y", 64);
  const SymRef Product = Ctx.mkMul(X, Y);
  Z3SolverOptions Options;
  Options.ResourceLimit = 1;
  Z3Solver Solver(Ctx, Options);
  EXPECT_EQ(Solver.checkDistinct(Product, Ctx.mkConst(64, 187)),
            SatResult::Unknown);
  EXPECT_TRUE(Solver.ok());
  EXPECT_FALSE(Solver.reasonUnknown().empty());
  EXPECT_TRUE(Solver.model().empty());
  EXPECT_EQ(z3CheckEqual(Ctx, Product, X, nullptr, Options),
            EquivResult::Unknown);
}

TEST(Z3Solver, ModelExtractionCanBeDisabledAndOldOutputIsCleared) {
  if (!Z3Solver::available())
    GTEST_SKIP() << "Z3 backend disabled";
  SymContext Ctx;
  const SymRef X = Ctx.mkVar("x", 129);
  Z3SolverOptions Options;
  Options.BuildModel = false;
  BitVectorModel Model;
  Model.set(Ctx.varId(X), llvm::APInt(129, 7));
  EXPECT_EQ(z3CheckSat(Ctx, X, &Model, Options), SatResult::Sat);
  EXPECT_TRUE(Model.empty());
  Model.set(Ctx.varId(X), llvm::APInt(129, 7));
  EXPECT_EQ(z3CheckEqual(Ctx, X, X, &Model), EquivResult::Equal);
  EXPECT_TRUE(Model.empty());
}
