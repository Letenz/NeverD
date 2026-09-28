//===- FiniteValuesTests.cpp - Optional finite projection guards ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/analysis/interpreter/FiniteValues.h"
#include "gtest/gtest.h"

#include <limits>

using namespace neverd::analysis;
using namespace neverd::analysis::detail;
using namespace neverd::symbolic;

namespace {

TEST(FiniteValues, UnrelatedPredicateLeavesWideProjectionInputUnconstrained) {
  SymContext Ctx;
  const SymRef Value = Ctx.mkVar("projection", 64);
  const SymRef Other = Ctx.mkVar("selector", 8);
  const SymRef Predicate = Ctx.mkEq(Other, Ctx.mkConst(8, 7));
  const size_t Nodes = Ctx.numNodes();
  EXPECT_TRUE(hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 32, 3));
  EXPECT_EQ(Ctx.numNodes(), Nodes);
}

TEST(FiniteValues, ExactSymbolAndItsExtractsRemainConstrained) {
  SymContext Ctx;
  const SymRef Value = Ctx.mkVar("projection", 64);
  EXPECT_FALSE(hasUnconstrainedProjectionInput(
      Ctx, Ctx.mkEq(Value, Ctx.mkConst(64, 7)), Value, 32, 100));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(
      Ctx, Ctx.mkEq(Ctx.mkExtract(Value, 8, 8), Ctx.mkConst(8, 7)), Value, 32,
      100));
  const SymRef Boolean = Ctx.mkVar("condition", 1);
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Boolean, Boolean, 1, 1));
}

TEST(FiniteValues, SymbolIdentityDoesNotDependOnDiagnosticNames) {
  SymContext Ctx;
  const SymRef Value =
      Ctx.mkInputVar("shared_name", 64, {SymInputKind::Register, 0, 8, 0});
  const SymRef Other =
      Ctx.mkInputVar("shared_name", 64, {SymInputKind::Register, 16, 8, 0});
  ASSERT_NE(Value, Other);
  EXPECT_TRUE(hasUnconstrainedProjectionInput(
      Ctx, Ctx.mkEq(Other, Ctx.mkConst(64, 7)), Value, 32, 100));
  const SymRef Overlapping =
      Ctx.mkInputVar("byte_lane", 8, {SymInputKind::Register, 1, 1, 0});
  EXPECT_FALSE(hasUnconstrainedProjectionInput(
      Ctx, Ctx.mkEq(Overlapping, Ctx.mkConst(8, 7)), Value, 32, 100));
}

TEST(FiniteValues, ProjectionTypeMustExceedTheRequestedLimit) {
  SymContext Ctx;
  const SymRef Predicate = Ctx.mkTrue();
  EXPECT_FALSE(hasUnconstrainedProjectionInput(
      Ctx, Predicate, Ctx.mkVar("five_bits", 5), 32, 1));
  EXPECT_TRUE(hasUnconstrainedProjectionInput(Ctx, Predicate,
                                              Ctx.mkVar("six_bits", 6), 32, 1));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate,
                                               Ctx.mkVar("boolean", 1), 2, 1));
  EXPECT_TRUE(hasUnconstrainedProjectionInput(Ctx, Predicate,
                                              Ctx.mkVar("boolean", 1), 1, 1));
  EXPECT_TRUE(hasUnconstrainedProjectionInput(
      Ctx, Predicate, Ctx.mkVar("full_word", 64),
      std::numeric_limits<uint32_t>::max(), 1));
}

TEST(FiniteValues, IncompleteDagWalkCannotProveIndependence) {
  SymContext Ctx;
  const SymRef Value = Ctx.mkVar("projection", 64);
  const SymRef Other = Ctx.mkVar("selector", 8);
  const SymRef Predicate = Ctx.mkEq(Other, Ctx.mkConst(8, 7));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 32, 0));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 32, 2));
  EXPECT_TRUE(hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 32, 3));

  // The shared sum appears below two comparisons. Its node and operands
  // count once, so the complete DAG has eight unique nodes, not a tree's 11.
  const SymRef Sum = Ctx.mkAdd(Ctx.mkVar("a", 8), Ctx.mkVar("b", 8));
  const SymRef Shared = Ctx.mkOr(Ctx.mkEq(Sum, Ctx.mkConst(8, 2)),
                                 Ctx.mkEq(Sum, Ctx.mkConst(8, 5)));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Shared, Value, 32, 7));
  EXPECT_TRUE(hasUnconstrainedProjectionInput(Ctx, Shared, Value, 32, 8));
}

TEST(FiniteValues, InvalidInputsAndCompositeValuesDoNotUseTheShortcut) {
  SymContext Ctx;
  const SymRef Value = Ctx.mkVar("projection", 64);
  const SymRef Predicate = Ctx.mkTrue();
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, {}, Value, 32, 100));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Value, Value, 32, 100));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate, {}, 32, 100));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 0, 100));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate,
                                               Ctx.mkConst(64, 7), 32, 100));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(
      Ctx, Predicate, Ctx.mkAdd(Value, Ctx.mkConst(64, 1)), 32, 100));
}

TEST(FiniteValues, ProjectionIndependenceDoesNotEstablishReachability) {
  SymContext Ctx;
  const SymRef Value = Ctx.mkVar("projection", 64);
  const SymRef Predicate = Ctx.mkFalse();
  ASSERT_TRUE(hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 32, 1));
  uint64_t Queries = 0;
  const auto Reachable =
      enumerateFiniteValues(Ctx, Predicate, {}, 32, {}, Queries);
  EXPECT_EQ(Reachable.Status, FiniteValueStatus::Complete);
  EXPECT_TRUE(Reachable.Tuples.empty());
  EXPECT_EQ(Queries, 0u);
}

} // namespace
