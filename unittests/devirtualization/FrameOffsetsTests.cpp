//===- FrameOffsetsTests.cpp - Entry-relative address proofs --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/analysis/core/FrameOffsets.h"
#include "gtest/gtest.h"

#include "neverd/symbolic/SymState.h"

using namespace neverd::analysis;
using namespace neverd::analysis::detail;
using namespace neverd::symbolic;

namespace {

SymRef residue(SymContext &Ctx, SymRef Root, uint64_t Mask, uint64_t Value) {
  return Ctx.mkEq(Ctx.mkAnd(Root, Ctx.mkConst(64, Mask)),
                  Ctx.mkConst(64, Value));
}

TEST(FrameOffsets, EveryAlignmentResidueKeepsArbitraryHighBits) {
  uint64_t Queries = 0;
  for (uint64_t Alignment : {2, 4, 8, 16, 32}) {
    for (uint64_t Low = 0; Low != Alignment; ++Low) {
      SCOPED_TRACE(Alignment);
      SCOPED_TRACE(Low);
      SymContext Ctx;
      const auto Root = Ctx.mkVar("entry", 64);
      for (uint64_t Bias : {uint64_t{0}, uint64_t{37}, uint64_t{0} - 53}) {
        const auto Value =
            Ctx.mkAdd(Ctx.mkAnd(Ctx.mkAdd(Root, Ctx.mkConst(64, Bias)),
                                Ctx.mkConst(64, uint64_t{0} - Alignment)),
                      Ctx.mkConst(64, 11));
        const auto Result =
            proveFrameOffset(Ctx, residue(Ctx, Root, Alignment - 1, Low), Value,
                             Root, {}, Queries);
        ASSERT_EQ(Result.Status, FrameOffsetStatus::Exact);
        EXPECT_EQ(Result.Offset, Bias + 11 - ((Low + Bias) & (Alignment - 1)));
      }
    }
  }
  EXPECT_GT(Queries, 0u);
}

TEST(FrameOffsets, ByteStateAndSplitArithmeticPreserveThePhysicalRoot) {
  SymContext Ctx;
  SymState State(Ctx);
  // The proof executor may obtain the root from independently named bytes.
  const auto Root = State.read(SymSpace::Register, 0, 8);
  const auto Adjusted = Ctx.mkAdd(Root, Ctx.mkConst(64, uint64_t{0} - 101));
  const auto Aligned = Ctx.mkConcat(
      Ctx.mkExtract(Adjusted, 8, 56),
      Ctx.mkAnd(Ctx.mkExtract(Adjusted, 0, 8), Ctx.mkConst(8, 0xe0)));
  State.write(SymSpace::Register, 8, Aligned);
  uint64_t Queries = 0;
  for (uint64_t Low = 0; Low != 32; ++Low) {
    const auto Result = proveFrameOffset(Ctx, residue(Ctx, Root, 31, Low),
                                         State.read(SymSpace::Register, 8, 8),
                                         Root, {}, Queries);
    ASSERT_EQ(Result.Status, FrameOffsetStatus::Exact);
    EXPECT_EQ(Result.Offset, uint64_t{0} - 101 - ((Low - 101) & 31));
  }
}

TEST(FrameOffsets, SplitAlignmentProjectsOnlyTheRemovedLowBits) {
  for (unsigned Split : {8U, 16U, 32U})
    for (uint64_t Alignment : {2, 4, 8, 16, 32})
      for (uint64_t Low = 0; Low != Alignment; ++Low)
        for (uint64_t Bias : {uint64_t{0}, uint64_t{173}, uint64_t{0} - 405}) {
          SCOPED_TRACE(Split);
          SCOPED_TRACE(Alignment);
          SCOPED_TRACE(Low);
          SCOPED_TRACE(Bias);
          SymContext Ctx;
          const auto Root = Ctx.mkVar("stack", 64);
          const auto Adjusted = Ctx.mkAdd(Root, Ctx.mkConst(64, Bias));
          const auto Aligned = Ctx.mkConcat(
              Ctx.mkExtract(Adjusted, Split, 64 - Split),
              Ctx.mkAnd(Ctx.mkExtract(Adjusted, 0, Split),
                        Ctx.mkConst(Split, uint64_t{0} - Alignment)));
          const auto Value = Ctx.mkAdd(Aligned, Ctx.mkConst(64, 27));
          SpecializationOptions Options;
          // A low-residue proof fits; bit-blasting a whole 64-bit subtraction
          // under the same ceiling does not. No high root bits are fixed.
          Options.MaxSolverGates = 256;
          uint64_t Queries = 0;
          const auto R =
              proveFrameOffset(Ctx, residue(Ctx, Root, Alignment - 1, Low),
                               Value, Root, Options, Queries);
          ASSERT_EQ(R.Status, FrameOffsetStatus::Exact);
          EXPECT_EQ(R.Offset, Bias + 27 - ((Low + Bias) & (Alignment - 1)));
        }
}

TEST(FrameOffsets, SplitAlignmentDoesNotMergeDifferentSourcesOrBiases) {
  SymContext Ctx;
  const auto Root = Ctx.mkVar("stack", 64);
  const auto Other = Ctx.mkVar("other", 64);
  const auto Predicate = residue(Ctx, Root, 15, 8);
  const auto High = Ctx.mkExtract(Ctx.mkAdd(Root, Ctx.mkConst(64, 128)), 8, 56);
  for (const auto LowSource : {Root, Other}) {
    const auto Low =
        Ctx.mkAnd(Ctx.mkExtract(LowSource, 0, 8), Ctx.mkConst(8, 0xf0));
    uint64_t Queries = 0;
    EXPECT_EQ(proveFrameOffset(Ctx, Predicate, Ctx.mkConcat(High, Low), Root,
                               {}, Queries)
                  .Status,
              FrameOffsetStatus::NonUnique);
  }
  // A sparse mask removes an unconstrained bit above the declared residue.
  const auto Sparse = Ctx.mkAnd(Root, Ctx.mkConst(64, ~uint64_t{0x21}));
  uint64_t Queries = 0;
  EXPECT_EQ(proveFrameOffset(Ctx, Predicate, Sparse, Root, {}, Queries).Status,
            FrameOffsetStatus::NonUnique);
  const auto Aligned = Ctx.mkAnd(Root, Ctx.mkConst(64, -16));
  EXPECT_EQ(
      proveFrameOffset(Ctx, Ctx.mkTrue(), Aligned, Root, {}, Queries).Status,
      FrameOffsetStatus::NonUnique);
}

TEST(FrameOffsets, NestedMaskedOffsetsPreserveModularCarriesAndWraparound) {
  for (uint64_t Entry :
       {uint64_t{0}, uint64_t{1}, uint64_t{15}, uint64_t{16}, uint64_t{127},
        uint64_t{255}, uint64_t{256}, uint64_t{0x7fffffffffffffff},
        UINT64_MAX - 255, UINT64_MAX})
    for (uint64_t Bias : {uint64_t{0}, uint64_t{173}, uint64_t{0} - 405}) {
      SCOPED_TRACE(Entry);
      SCOPED_TRACE(Bias);
      SymContext Ctx;
      const auto Root = Ctx.mkVar("stack", 64);
      const auto Adjusted = Ctx.mkAdd(Root, Ctx.mkConst(64, Bias));
      const auto Split = Ctx.mkConcat(
          Ctx.mkExtract(Adjusted, 8, 56),
          Ctx.mkAnd(Ctx.mkExtract(Adjusted, 0, 8), Ctx.mkConst(8, 0xf0)));
      const auto Value = Ctx.mkAdd(
          Ctx.mkAnd(Ctx.mkAdd(Split, Ctx.mkConst(64, 7)), Ctx.mkConst(64, -32)),
          Ctx.mkConst(64, 11));
      uint64_t Queries = 0;
      const auto Result =
          proveFrameOffset(Ctx, Ctx.mkEq(Root, Ctx.mkConst(64, Entry)), Value,
                           Root, {}, Queries);
      ASSERT_EQ(Result.Status, FrameOffsetStatus::Exact);
      const uint64_t First = (Entry + Bias) & ~uint64_t{15};
      const uint64_t Expected = ((First + 7) & ~uint64_t{31}) + 11;
      EXPECT_EQ(Result.Offset, Expected - Entry);
    }
}

TEST(FrameOffsets, RemainderConstructionAndIncompleteQueriesStayBudgeted) {
  SymContext Ctx;
  const auto Root = Ctx.mkVar("stack", 64);
  const auto Value = Ctx.mkAdd(
      Ctx.mkAnd(Ctx.mkAdd(Root, Ctx.mkConst(64, 173)), Ctx.mkConst(64, -16)),
      Ctx.mkConst(64, 27));
  const auto Predicate = residue(Ctx, Root, 15, 8);
  SpecializationOptions Options;
  Options.MaxSymbolicNodes = Ctx.numNodes();
  uint64_t Queries = 0;
  EXPECT_EQ(
      proveFrameOffset(Ctx, Predicate, Value, Root, Options, Queries).Status,
      FrameOffsetStatus::BudgetExceeded);
  EXPECT_EQ(Queries, 0U);
  Options.MaxSymbolicNodes = 65536;
  Options.MaxSolverQueries = 1;
  EXPECT_EQ(
      proveFrameOffset(Ctx, Predicate, Value, Root, Options, Queries).Status,
      FrameOffsetStatus::BudgetExceeded);
  EXPECT_EQ(Queries, 1U);
  Options.MaxSolverQueries = 3;
  EXPECT_EQ(
      proveFrameOffset(Ctx, Predicate, Value, Root, Options, Queries).Status,
      FrameOffsetStatus::Exact);
  EXPECT_EQ(Queries, 3U);
}

TEST(FrameOffsets, UnpartitionedAndIndependentValuesDoNotInventOffsets) {
  SymContext Ctx;
  const auto Root = Ctx.mkVar("entry", 64);
  const auto Aligned = Ctx.mkAnd(Root, Ctx.mkConst(64, uint64_t{0} - 16));
  uint64_t Queries = 0;
  for (auto Value : {Aligned, Ctx.mkVar("other", 64),
                     Ctx.mkAnd(Root, Ctx.mkVar("mask", 64))}) {
    const auto Result =
        proveFrameOffset(Ctx, Ctx.mkTrue(), Value, Root, {}, Queries);
    EXPECT_EQ(Result.Status, FrameOffsetStatus::NonUnique);
  }
}

TEST(FrameOffsets, PartialModelsAndExhaustedWorkSupplyNoOffset) {
  SymContext Ctx;
  const auto Root = Ctx.mkVar("entry", 64);
  const auto Value = Ctx.mkAnd(Root, Ctx.mkConst(64, uint64_t{0} - 8));
  const auto Predicate = residue(Ctx, Root, 7, 3);
  SpecializationOptions Options;
  uint64_t Queries = 0;
  Options.MaxSolverQueries = 1;
  EXPECT_EQ(
      proveFrameOffset(Ctx, Predicate, Value, Root, Options, Queries).Status,
      FrameOffsetStatus::BudgetExceeded);
  EXPECT_EQ(Queries, 1u);
  Options.MaxSolverQueries = 3;
  EXPECT_EQ(
      proveFrameOffset(Ctx, Predicate, Value, Root, Options, Queries).Status,
      FrameOffsetStatus::Exact);
  EXPECT_EQ(Queries, 3u);
  Options.MaxSymbolicNodes = Ctx.numNodes() - 1;
  EXPECT_EQ(
      proveFrameOffset(Ctx, Predicate, Root, Root, Options, Queries).Status,
      FrameOffsetStatus::BudgetExceeded);
}

TEST(FrameOffsets, ExactIdentityIsSeparateFromReachability) {
  SymContext Ctx;
  const auto Root = Ctx.mkVar("entry", 64);
  const auto Value = Ctx.mkAdd(Root, Ctx.mkConst(64, 8));
  uint64_t Queries = 0;
  const auto UnknownPredicate = Ctx.mkVar("condition", 1);
  EXPECT_EQ(
      proveFrameOffset(Ctx, UnknownPredicate, Value, Root, {}, Queries).Status,
      FrameOffsetStatus::Exact);
  EXPECT_EQ(Queries, 0u);
  EXPECT_EQ(
      proveFrameOffset(Ctx, Ctx.mkFalse(), Value, Root, {}, Queries).Status,
      FrameOffsetStatus::Infeasible);
  const auto Contradiction = Ctx.mkAnd(Ctx.mkUlt(Root, Ctx.mkConst(64, 3)),
                                       Ctx.mkUlt(Ctx.mkConst(64, 5), Root));
  EXPECT_EQ(proveFrameOffset(Ctx, Contradiction,
                             Ctx.mkAnd(Root, Ctx.mkConst(64, -16)), Root, {},
                             Queries)
                .Status,
            FrameOffsetStatus::Infeasible);
  EXPECT_GT(Queries, 0u);
}

TEST(FrameOffsets, CacheRetainsCompletePredicateAndRootRelationships) {
  FiniteQueryCache Cache(65536);
  SymContext Ctx;
  const auto Root = Ctx.mkVar("entry", 64);
  const auto Value = Ctx.mkAnd(Root, Ctx.mkConst(64, uint64_t{0} - 16));
  uint64_t Queries = 0;
  const auto Prove = [&](uint64_t Low) {
    return proveFrameOffset(Ctx, residue(Ctx, Root, 15, Low), Value, Root, {},
                            Queries, &Cache);
  };
  ASSERT_EQ(Prove(5).Status, FrameOffsetStatus::Exact);
  const auto Before = Queries;
  EXPECT_EQ(Prove(5).Offset, uint64_t{0} - 5);
  EXPECT_EQ(Queries, Before);
  EXPECT_EQ(Prove(6).Offset, uint64_t{0} - 6);
  EXPECT_GT(Queries, Before);
  EXPECT_EQ(
      proveFrameOffset(Ctx, Ctx.mkTrue(), Value, Root, {}, Queries, &Cache)
          .Status,
      FrameOffsetStatus::NonUnique);
}

TEST(FrameOffsets, CanonicalAccessesShareBytesAndRetainAliasInvalidation) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto Root = Ctx.mkVar("entry", 64);
  const auto Pointer =
      Ctx.mkAnd(Ctx.mkAdd(Root, Ctx.mkConst(64, -32)), Ctx.mkConst(64, -16));
  uint64_t Queries = 0;
  const auto Offset = proveFrameOffset(Ctx, residue(Ctx, Root, 15, 9), Pointer,
                                       Root, {}, Queries);
  ASSERT_EQ(Offset.Status, FrameOffsetStatus::Exact);
  const auto Canonical = Ctx.mkAdd(Root, Ctx.mkConst(64, Offset.Offset));
  State.store(Canonical, Root);
  EXPECT_EQ(State.load(Ctx.mkAdd(Root, Ctx.mkConst(64, -41)), 8), Root);
  State.store(Ctx.mkAdd(Canonical, Ctx.mkConst(64, 1)), Ctx.mkConst(8, 7));
  EXPECT_NE(State.load(Canonical, 8), Root);
  State.store(Ctx.mkVar("possible_alias", 64), Ctx.mkConst(8, 2));
  EXPECT_FALSE(Ctx.asConst(State.load(Canonical, 8)));
}

TEST(FrameOffsets, InvalidShapesAreRejectedBeforeSymbolicOperations) {
  SymContext Ctx;
  const auto Root = Ctx.mkVar("entry", 64);
  uint64_t Queries = 0;
  EXPECT_EQ(proveFrameOffset(Ctx, {}, Root, Root, {}, Queries).Status,
            FrameOffsetStatus::Invalid);
  EXPECT_EQ(proveFrameOffset(Ctx, Root, Root, Root, {}, Queries).Status,
            FrameOffsetStatus::Invalid);
  EXPECT_EQ(proveFrameOffset(Ctx, Ctx.mkTrue(), {}, Root, {}, Queries).Status,
            FrameOffsetStatus::Invalid);
  EXPECT_EQ(
      proveFrameOffset(Ctx, Ctx.mkTrue(), Ctx.mkConst(32, 0), Root, {}, Queries)
          .Status,
      FrameOffsetStatus::Invalid);
  EXPECT_EQ(Queries, 0u);
}

} // namespace
