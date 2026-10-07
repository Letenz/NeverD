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

TEST(FrameOffsets, SymbolicAddendsPreserveAlignmentAndWholeIndex) {
  for (unsigned Width : {8U, 64U})
    for (unsigned Split : {0U, 8U, 32U})
      for (bool IndexFirst : {false, true})
        for (uint64_t Bias : {uint64_t{37}, uint64_t{0} - 101}) {
          SCOPED_TRACE(Width);
          SCOPED_TRACE(Split);
          SCOPED_TRACE(IndexFirst);
          SCOPED_TRACE(Bias);
          SymContext Ctx;
          SymRef Root, Index;
          if (IndexFirst) {
            Index = Ctx.mkVar("index", Width);
            Root = Ctx.mkVar("stack", 64);
          } else {
            Root = Ctx.mkVar("stack", 64);
            Index = Ctx.mkVar("index", Width);
          }
          const auto Adjusted = Ctx.mkAdd(Root, Ctx.mkConst(64, Bias));
          const auto Aligned =
              Split ? Ctx.mkConcat(
                          Ctx.mkExtract(Adjusted, Split, 64 - Split),
                          Ctx.mkAnd(Ctx.mkExtract(Adjusted, 0, Split),
                                    Ctx.mkConst(Split, uint64_t{0} - 16)))
                    : Ctx.mkAnd(Adjusted, Ctx.mkConst(64, uint64_t{0} - 16));
          const auto Value = Ctx.mkAdd(Aligned, Ctx.mkZExtOrTrunc(Index, 64));
          SpecializationOptions Options;
          Options.MaxSolverGates = 512;
          for (uint64_t Low : {0U, 9U, 15U}) {
            const uint64_t Number = Width == 8 ? 253 : UINT64_MAX - 6;
            const auto Predicate =
                Ctx.mkAnd(residue(Ctx, Root, 15, Low),
                          Ctx.mkEq(Index, Ctx.mkConst(Width, Number)));
            uint64_t Queries = 0;
            const auto Good =
                proveFrameOffset(Ctx, Predicate, Value, Root, Options, Queries);
            ASSERT_EQ(Good.Status, FrameOffsetStatus::Exact);
            EXPECT_EQ(Good.Offset, Bias + Number - ((Low + Bias) & 15));
            EXPECT_EQ(Queries, 2U);
          }
        }
}

TEST(FrameOffsets, SymbolicAddendsDoNotDiscardConstraintsOrRootDependence) {
  SymContext Ctx;
  const auto Root = Ctx.mkVar("stack", 64);
  const auto Other = Ctx.mkVar("other", 64);
  const auto Index = Ctx.mkVar("index", 8);
  const auto Aligned = Ctx.mkAnd(Root, Ctx.mkConst(64, -16));
  const auto Predicate = residue(Ctx, Root, 15, 8);
  for (const auto Addend : {Root, Ctx.mkZExt(Ctx.mkExtract(Root, 0, 8), 64),
                            Ctx.mkZExt(Index, 64)}) {
    uint64_t Queries = 0;
    EXPECT_EQ(proveFrameOffset(Ctx, Predicate, Ctx.mkAdd(Aligned, Addend), Root,
                               {}, Queries)
                  .Status,
              FrameOffsetStatus::NonUnique);
    EXPECT_EQ(Queries, 2U);
  }
  const auto Fixed = Ctx.mkAnd(Predicate, Ctx.mkEq(Index, Ctx.mkConst(8, 17)));
  const auto WrongRoot =
      Ctx.mkAdd(Ctx.mkAnd(Other, Ctx.mkConst(64, -16)), Ctx.mkZExt(Index, 64));
  uint64_t Queries = 0;
  EXPECT_EQ(proveFrameOffset(Ctx, Fixed, WrongRoot, Root, {}, Queries).Status,
            FrameOffsetStatus::NonUnique);
  const auto Guard = Ctx.mkVar("unrelated_guard", 8);
  const auto Contradiction = Ctx.mkAnd(Ctx.mkUlt(Guard, Ctx.mkConst(8, 3)),
                                       Ctx.mkUlt(Ctx.mkConst(8, 5), Guard));
  EXPECT_EQ(proveFrameOffset(Ctx, Ctx.mkAnd(Fixed, Contradiction),
                             Ctx.mkAdd(Aligned, Ctx.mkZExt(Index, 64)), Root,
                             {}, Queries)
                .Status,
            FrameOffsetStatus::Infeasible);
}

TEST(FrameOffsets, SymbolicAddendProofsStillRequireCompleteBudgets) {
  for (unsigned Case = 0; Case != 4; ++Case) {
    SymContext Ctx;
    const auto Root = Ctx.mkVar("stack", 64), Index = Ctx.mkVar("index", 8);
    const auto Value =
        Ctx.mkAdd(Ctx.mkAnd(Root, Ctx.mkConst(64, -16)), Ctx.mkZExt(Index, 64));
    const auto Predicate = Ctx.mkAnd(residue(Ctx, Root, 15, 8),
                                     Ctx.mkEq(Index, Ctx.mkConst(8, 17)));
    SpecializationOptions Options;
    Options.MaxSolverGates = Case == 0 ? 1 : 512;
    Options.MaxSymbolicNodes = Case == 1 ? Ctx.numNodes() : 65536;
    Options.MaxSolverQueries = Case == 2 ? 1 : 2;
    uint64_t Queries = 0;
    const auto Result =
        proveFrameOffset(Ctx, Predicate, Value, Root, Options, Queries);
    EXPECT_EQ(Result.Status, Case == 3 ? FrameOffsetStatus::Exact
                                       : FrameOffsetStatus::BudgetExceeded);
    EXPECT_EQ(Queries, Case < 2 ? 0U : Case == 2 ? 1U : 2U);
    if (Case == 3)
      EXPECT_EQ(Result.Offset, 9U);
  }
}

TEST(FrameOffsets, DomainEncodingReuseKeepsFullFrameProofsAndBudgets) {
  SymContext Ctx;
  const auto Root = Ctx.mkVar("entry", 64);
  const auto Pointer = Ctx.mkVar("pointer", 64);
  neverd::solver::SolverOptions Settings;
  FiniteDomainEncoding Encoding(Ctx, Settings);
  for (unsigned Offset : {7U, 7U, 19U, 7U}) {
    const auto Predicate =
        Ctx.mkEq(Pointer, Ctx.mkAdd(Root, Ctx.mkConst(64, Offset)));
    for (unsigned Bias = 0; Bias != 8; ++Bias) {
      const auto Value = Ctx.mkAdd(Pointer, Ctx.mkConst(64, Bias));
      uint64_t FreshQueries = 0, CopyQueries = 0;
      const auto Fresh = proveFrameOffset(Ctx, Predicate, Value, Root, Settings,
                                          2, 100000, FreshQueries);
      const auto Copy = proveFrameOffset(Encoding, Predicate, Value, Root, 2,
                                         100000, CopyQueries);
      EXPECT_EQ(Copy.Status, FrameOffsetStatus::Exact);
      EXPECT_EQ(Copy.Status, Fresh.Status);
      EXPECT_EQ(Copy.Offset, Offset + Bias);
      EXPECT_EQ(Copy.Offset, Fresh.Offset);
      EXPECT_EQ(CopyQueries, FreshQueries);
      EXPECT_EQ(CopyQueries, 2U);
      uint64_t LimitedQueries = 0;
      EXPECT_EQ(proveFrameOffset(Encoding, Predicate, Value, Root, 1, 100000,
                                 LimitedQueries)
                    .Status,
                FrameOffsetStatus::BudgetExceeded);
      EXPECT_EQ(LimitedQueries, 1U);
    }
  }
  uint64_t Queries = 0;
  EXPECT_EQ(proveFrameOffset(Encoding, Ctx.mkTrue(), Pointer, Root, 3, 100000,
                             Queries)
                .Status,
            FrameOffsetStatus::NonUnique);
  Queries = 0;
  EXPECT_EQ(proveFrameOffset(Encoding, Ctx.mkFalse(), Pointer, Root, 2, 100000,
                             Queries)
                .Status,
            FrameOffsetStatus::Infeasible);
}

TEST(FrameOffsets, FrameAndJointTargetProjectionsKeepIndependentSearches) {
  SymContext Ctx;
  const auto Root = Ctx.mkVar("stack", 64);
  const auto X = Ctx.mkVar("x", 4), Y = Ctx.mkVar("y", 4);
  neverd::solver::SolverOptions Settings;
  Settings.BuildModel = false;
  FiniteDomainEncoding Encoding(Ctx, Settings);
  for (unsigned Low : {8U, 24U, 8U}) {
    const auto Predicate = Ctx.mkAnd(
        Ctx.mkEq(Ctx.mkAnd(Root, Ctx.mkConst(64, 127)), Ctx.mkConst(64, Low)),
        Ctx.mkEq(Ctx.mkAdd(X, Y), Ctx.mkConst(4, Low == 24 ? 3 : 7)));
    for (unsigned Round = 0; Round != 4; ++Round) {
      const auto Address =
          Ctx.mkAdd(Ctx.mkAnd(Ctx.mkAdd(Root, Ctx.mkConst(64, Round + 1)),
                              Ctx.mkConst(64, uint64_t{0} - 128)),
                    Ctx.mkConst(64, 11));
      uint64_t FrameQueries = 0, FreshFrameQueries = 0;
      const auto Frame = proveFrameOffset(Encoding, Predicate, Address, Root, 2,
                                          100000, FrameQueries);
      const auto FreshFrame =
          proveFrameOffset(Ctx, Predicate, Address, Root, Settings, 2, 100000,
                           FreshFrameQueries);
      ASSERT_EQ(Frame.Status, FrameOffsetStatus::Exact);
      EXPECT_EQ(Frame.Status, FreshFrame.Status);
      EXPECT_EQ(Frame.Offset, uint64_t{11} - Low);
      EXPECT_EQ(Frame.Offset, FreshFrame.Offset);
      EXPECT_EQ(FrameQueries, FreshFrameQueries);
      EXPECT_EQ(FrameQueries, 2U);

      const auto Target = Ctx.mkAdd(Ctx.mkZExt(X, 64), Ctx.mkConst(64, 0x7000));
      uint64_t Queries = 0, FreshQueries = 0;
      std::vector<std::vector<uint64_t>> Seen, FreshSeen;
      const auto MaxQueries = Round == 1 ? 16U : 17U;
      const auto MaxValues = Round == 2 ? 15U : 16U;
      const auto Result = enumerateFiniteValues(
          Encoding, Predicate, {Target, Y}, MaxValues, MaxQueries, 100000,
          Queries, [&](llvm::ArrayRef<uint64_t> Tuple) {
            Seen.emplace_back(Tuple.begin(), Tuple.end());
            return true;
          });
      const auto Fresh = enumerateFiniteValues(
          Ctx, Predicate, {Target, Y}, MaxValues, Settings, MaxQueries, 100000,
          FreshQueries, [&](llvm::ArrayRef<uint64_t> Tuple) {
            FreshSeen.emplace_back(Tuple.begin(), Tuple.end());
            return true;
          });
      EXPECT_EQ(Result.Status, Fresh.Status);
      EXPECT_EQ(Result.Tuples, Fresh.Tuples);
      EXPECT_EQ(Queries, FreshQueries);
      EXPECT_EQ(Seen, FreshSeen);
      if (Round == 1 || Round == 2) {
        EXPECT_EQ(Result.Status, Round == 1
                                     ? FiniteValueStatus::QueryBudgetExceeded
                                     : FiniteValueStatus::TooManyValues);
        EXPECT_TRUE(Result.Tuples.empty());
        EXPECT_EQ(Queries, 16U);
      } else {
        ASSERT_EQ(Result.Status, FiniteValueStatus::Complete);
        ASSERT_EQ(Result.Tuples.size(), 16U);
        EXPECT_EQ(Queries, 17U);
        for (unsigned I = 0; I != 16; ++I) {
          EXPECT_EQ(Result.Tuples[I][0], 0x7000 + I);
          EXPECT_EQ(Result.Tuples[I][1], ((Low == 24 ? 3U : 7U) - I) & 15);
        }
      }
    }
  }
}

} // namespace
