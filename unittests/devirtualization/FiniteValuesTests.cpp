//===- FiniteValuesTests.cpp - Optional finite projection guards ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/analysis/core/FiniteValues.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>

using namespace neverd::analysis;
using namespace neverd::analysis::detail;
using namespace neverd::symbolic;

namespace {

TEST(FiniteValues, InvalidPredicateReferencesFailBeforeNodeAccess) {
  for (bool UseEncoding : {false, true}) {
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", 8);
    FiniteDomainEncoding Encoding(Ctx, {});
    for (SymRef Predicate :
         {SymRef{}, X, SymRef(Ctx.numNodes()), SymRef(0xfffffffe)}) {
      SCOPED_TRACE(UseEncoding);
      SCOPED_TRACE(Predicate.index());
      const auto Nodes = Ctx.numNodes();
      uint64_t Queries = 7;
      unsigned Observed = 0;
      const auto Observe = [&](llvm::ArrayRef<uint64_t>) {
        ++Observed;
        return true;
      };
      const auto Result =
          UseEncoding ? enumerateFiniteValues(Encoding, Predicate, {X}, 1, 20,
                                              10000, Queries, Observe)
                      : enumerateFiniteValues(Ctx, Predicate, {X}, 1, {}, 20,
                                              10000, Queries, Observe);
      EXPECT_EQ(Result.Status, FiniteValueStatus::Invalid);
      EXPECT_TRUE(Result.Tuples.empty());
      EXPECT_EQ(Queries, 7u);
      EXPECT_EQ(Observed, 0u);
      EXPECT_EQ(Ctx.numNodes(), Nodes);
    }
  }
}

TEST(FiniteValues, InvalidProjectionReferencesPrecedeEveryFastPath) {
  for (bool UseEncoding : {false, true}) {
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", 8), Seven = Ctx.mkConst(8, 7);
    const auto Symbolic = Ctx.mkEq(X, Seven);
    const auto False = Ctx.mkFalse(), True = Ctx.mkTrue();
    const auto Wide = Ctx.mkVar("wide", 128);
    FiniteDomainEncoding Encoding(Ctx, {});
    for (SymRef Predicate : {False, True, Symbolic}) {
      for (SymRef Value :
           {SymRef{}, Wide, SymRef(Ctx.numNodes()), SymRef(0xfffffffe)}) {
        SCOPED_TRACE(UseEncoding);
        SCOPED_TRACE(Predicate.index());
        SCOPED_TRACE(Value.index());
        const auto Nodes = Ctx.numNodes();
        uint64_t Queries = 7;
        unsigned Observed = 0;
        const auto Observe = [&](llvm::ArrayRef<uint64_t>) {
          ++Observed;
          return true;
        };
        const auto Result =
            UseEncoding
                ? enumerateFiniteValues(Encoding, Predicate, {Seven, Value}, 1,
                                        20, 10000, Queries, Observe)
                : enumerateFiniteValues(Ctx, Predicate, {Seven, Value}, 1, {},
                                        20, 10000, Queries, Observe);
        EXPECT_EQ(Result.Status, FiniteValueStatus::Invalid);
        EXPECT_TRUE(Result.Tuples.empty());
        EXPECT_EQ(Queries, 7u);
        EXPECT_EQ(Observed, 0u);
        EXPECT_EQ(Ctx.numNodes(), Nodes);
      }
    }
    uint64_t Queries = 0;
    const auto Good =
        enumerateFiniteValues(Encoding, Symbolic, {X}, 1, 2, 10000, Queries);
    EXPECT_EQ(Good.Status, FiniteValueStatus::Complete);
    EXPECT_EQ(Good.Tuples, (std::vector<std::vector<uint64_t>>{{7}}));
    EXPECT_EQ(Queries, 2u);
  }
}

TEST(FiniteValues, ObserverCanAbandonButCannotFilterTheDomain) {
  for (unsigned RejectAt : {0u, 1u, 3u}) {
    SCOPED_TRACE(RejectAt);
    SymContext Ctx;
    const auto Value = Ctx.mkVar("candidate", 2);
    const auto Predicate = Ctx.mkNot(Ctx.mkEq(Value, Ctx.mkConst(2, 3)));
    uint64_t Queries = 0;
    unsigned Seen = 0;
    const auto Result = enumerateFiniteValues(
        Ctx, Predicate, {Value}, 3, SpecializationOptions{}, Queries,
        [&](llvm::ArrayRef<uint64_t> Tuple) {
          EXPECT_EQ(Tuple.size(), 1u);
          EXPECT_LT(Tuple.front(), 3u);
          return ++Seen != RejectAt;
        });
    if (RejectAt) {
      EXPECT_EQ(Result.Status, FiniteValueStatus::Unknown);
      EXPECT_TRUE(Result.Tuples.empty());
      EXPECT_EQ(Queries, RejectAt);
      EXPECT_EQ(Seen, RejectAt);
    } else {
      EXPECT_EQ(Result.Status, FiniteValueStatus::Complete);
      EXPECT_EQ(Result.Tuples,
                (std::vector<std::vector<uint64_t>>{{0}, {1}, {2}}));
      EXPECT_EQ(Queries, 4u);
      EXPECT_EQ(Seen, 3u);
    }
  }
}

TEST(FiniteValues, ObserverHandlesConstantsAndNeverSeesInfeasibleTuples) {
  SymContext Ctx;
  uint64_t Queries = 0;
  unsigned Seen = 0;
  const auto Reject = [&](llvm::ArrayRef<uint64_t> Tuple) {
    EXPECT_EQ(Tuple.front(), 23u);
    ++Seen;
    return false;
  };
  const auto Constant =
      enumerateFiniteValues(Ctx, Ctx.mkTrue(), {Ctx.mkConst(8, 23)}, 1,
                            SpecializationOptions{}, Queries, Reject);
  EXPECT_EQ(Constant.Status, FiniteValueStatus::Unknown);
  EXPECT_TRUE(Constant.Tuples.empty());
  EXPECT_EQ(Queries, 0u);
  EXPECT_EQ(Seen, 1u);
  const auto Infeasible =
      enumerateFiniteValues(Ctx, Ctx.mkFalse(), {Ctx.mkConst(8, 23)}, 1,
                            SpecializationOptions{}, Queries, Reject);
  EXPECT_EQ(Infeasible.Status, FiniteValueStatus::Complete);
  EXPECT_TRUE(Infeasible.Tuples.empty());
  EXPECT_EQ(Seen, 1u);
  const auto EmptyProjection =
      enumerateFiniteValues(Ctx, Ctx.mkTrue(), {}, 1, SpecializationOptions{},
                            Queries, [&](llvm::ArrayRef<uint64_t> Tuple) {
                              EXPECT_TRUE(Tuple.empty());
                              ++Seen;
                              return false;
                            });
  EXPECT_EQ(EmptyProjection.Status, FiniteValueStatus::Unknown);
  EXPECT_TRUE(EmptyProjection.Tuples.empty());
  EXPECT_EQ(Queries, 0u);
  EXPECT_EQ(Seen, 2u);
}

TEST(FiniteValues, AcceptedObservationsStillNeedFinalUnsatWithinBudget) {
  SymContext Ctx;
  const auto Value = Ctx.mkVar("candidate", 2);
  const auto Predicate = Ctx.mkNot(Ctx.mkEq(Value, Ctx.mkConst(2, 3)));
  SpecializationOptions Options;
  Options.MaxSolverQueries = 3;
  uint64_t Queries = 0;
  unsigned Seen = 0;
  const auto Result =
      enumerateFiniteValues(Ctx, Predicate, {Value}, 3, Options, Queries,
                            [&](llvm::ArrayRef<uint64_t>) {
                              ++Seen;
                              return true;
                            });
  EXPECT_EQ(Seen, 3u);
  EXPECT_EQ(Queries, 3u);
  EXPECT_EQ(Result.Status, FiniteValueStatus::QueryBudgetExceeded);
  EXPECT_TRUE(Result.Tuples.empty());
}

TEST(FiniteValues, UnrelatedPredicateLeavesWideProjectionInputUnconstrained) {
  SymContext Ctx;
  const SymRef Value = Ctx.mkVar("projection", 64);
  const SymRef Other = Ctx.mkVar("selector", 8);
  const SymRef Predicate = Ctx.mkEq(Other, Ctx.mkConst(8, 7));
  const size_t Nodes = Ctx.numNodes();
  EXPECT_TRUE(hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 32, 9));
  EXPECT_EQ(Ctx.numNodes(), Nodes);
}

TEST(FiniteValues, PartiallyConstrainedInputRetainsIndependentPayloadBits) {
  SymContext Ctx;
  for (uint32_t Width : {8U, 48U, 56U, 64U}) {
    const auto Word = Ctx.mkVar("input" + std::to_string(Width), Width);
    const auto Guard = Ctx.mkEq(Ctx.mkExtract(Word, 0, 2), Ctx.mkConst(2, 1));
    const auto Payload = Ctx.mkAnd(Word, Ctx.mkConst(Width, 0xfc));
    const auto Nodes = Ctx.numNodes();
    EXPECT_TRUE(hasUnconstrainedProjectionInput(Ctx, Guard, Payload, 63, 1000));
    EXPECT_FALSE(
        hasUnconstrainedProjectionInput(Ctx, Guard, Payload, 64, 1000));
    EXPECT_EQ(Ctx.numNodes(), Nodes);
  }
}

TEST(FiniteValues, PredicatesConstrainOnlyTheirDemandedInputBits) {
  SymContext Ctx;
  const SymRef Value = Ctx.mkVar("projection", 64);
  EXPECT_FALSE(hasUnconstrainedProjectionInput(
      Ctx, Ctx.mkEq(Value, Ctx.mkConst(64, 7)), Value, 32, 100));
  EXPECT_TRUE(hasUnconstrainedProjectionInput(
      Ctx, Ctx.mkEq(Ctx.mkExtract(Value, 8, 8), Ctx.mkConst(8, 7)), Value, 32,
      100));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(
      Ctx, Ctx.mkEq(Ctx.mkExtract(Value, 8, 8), Ctx.mkConst(8, 7)),
      Ctx.mkExtract(Value, 8, 8), 32, 100));
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
  EXPECT_TRUE(hasUnconstrainedProjectionInput(
      Ctx, Ctx.mkEq(Overlapping, Ctx.mkConst(8, 7)), Value, 32, 100));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(
      Ctx, Ctx.mkEq(Overlapping, Ctx.mkConst(8, 7)), Ctx.mkExtract(Value, 8, 8),
      32, 100));
}

TEST(FiniteValues, ProjectionTypeMustExceedTheRequestedLimit) {
  SymContext Ctx;
  const SymRef Predicate = Ctx.mkTrue();
  EXPECT_FALSE(hasUnconstrainedProjectionInput(
      Ctx, Predicate, Ctx.mkVar("five_bits", 5), 32, 1));
  EXPECT_TRUE(hasUnconstrainedProjectionInput(Ctx, Predicate,
                                              Ctx.mkVar("six_bits", 6), 32, 7));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate,
                                               Ctx.mkVar("boolean", 1), 2, 1));
  EXPECT_TRUE(hasUnconstrainedProjectionInput(Ctx, Predicate,
                                              Ctx.mkVar("boolean", 1), 1, 2));
  EXPECT_TRUE(hasUnconstrainedProjectionInput(
      Ctx, Predicate, Ctx.mkVar("full_word", 64),
      std::numeric_limits<uint32_t>::max(), 33));
}

TEST(FiniteValues, IncompleteDagWalkCannotProveIndependence) {
  SymContext Ctx;
  const SymRef Value = Ctx.mkVar("projection", 64);
  const SymRef Other = Ctx.mkVar("selector", 8);
  const SymRef Predicate = Ctx.mkEq(Other, Ctx.mkConst(8, 7));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 32, 0));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 32, 8));
  EXPECT_TRUE(hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 32, 9));

  // The shared sum appears below two comparisons. Its node and operands
  // are visited once. Predicate edges and six independent value bits share
  // the same work budget; expanding the shared sum twice would exceed it.
  const SymRef Sum = Ctx.mkAdd(Ctx.mkVar("a", 8), Ctx.mkVar("b", 8));
  const SymRef Shared = Ctx.mkOr(Ctx.mkEq(Sum, Ctx.mkConst(8, 2)),
                                 Ctx.mkEq(Sum, Ctx.mkConst(8, 5)));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Shared, Value, 32, 14));
  EXPECT_TRUE(hasUnconstrainedProjectionInput(Ctx, Shared, Value, 32, 15));
}

TEST(FiniteValues, InvalidInputsAndArithmeticDoNotUseTheShortcut) {
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
  ASSERT_TRUE(hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 32, 7));
  uint64_t Queries = 0;
  const auto Reachable =
      enumerateFiniteValues(Ctx, Predicate, {}, 32, {}, Queries);
  EXPECT_EQ(Reachable.Status, FiniteValueStatus::Complete);
  EXPECT_TRUE(Reachable.Tuples.empty());
  EXPECT_EQ(Queries, 0u);
}

TEST(FiniteValues, ConstantBitwiseMasksPreserveOnlyUnforcedSourceBits) {
  SymContext Ctx;
  const SymRef Word = Ctx.mkVar("word", 64);
  const SymRef Predicate = Ctx.mkTrue();
  constexpr uint64_t SixBits = 0x8000800080008081;
  constexpr uint64_t FiveBits = SixBits & ~uint64_t{1};
  for (uint64_t Mask : {FiveBits, SixBits}) {
    const bool Exceeds = Mask == SixBits;
    for (SymRef Value : {Ctx.mkAnd(Word, Ctx.mkConst(64, Mask)),
                         Ctx.mkOr(Word, Ctx.mkConst(64, ~Mask)),
                         Ctx.mkXor(Ctx.mkAnd(Word, Ctx.mkConst(64, Mask)),
                                   Ctx.mkConst(64, 0x987654321abcdef0)),
                         Ctx.mkNot(Ctx.mkAnd(Word, Ctx.mkConst(64, Mask)))})
      EXPECT_EQ(
          hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 32, 1000),
          Exceeds);
  }
  EXPECT_FALSE(hasUnconstrainedProjectionInput(
      Ctx, Predicate, Ctx.mkAnd(Word, Ctx.mkConst(64, SixBits)), 32, 20));
}

TEST(FiniteValues, ExtractAndConcatCountIndependentSourceBitIdentities) {
  SymContext Ctx;
  const SymRef Word = Ctx.mkVar("word", 16);
  const SymRef Other = Ctx.mkVar("other", 3);
  const SymRef Predicate = Ctx.mkTrue();
  EXPECT_TRUE(hasUnconstrainedProjectionInput(
      Ctx, Predicate, Ctx.mkExtract(Word, 7, 6), 32, 100));
  const SymRef Split = Ctx.mkConcat(Ctx.mkExtract(Word, 0, 3), Other);
  EXPECT_TRUE(hasUnconstrainedProjectionInput(Ctx, Predicate, Split, 32, 100));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(
      Ctx, Ctx.mkEq(Other, Ctx.mkConst(3, 0)), Split, 8, 100));
  // Overlapping views expose six distinct bits, not eight. Repeating a view
  // or complementing it never manufactures another independent source bit.
  const SymRef Overlap =
      Ctx.mkConcat(Ctx.mkExtract(Word, 0, 4), Ctx.mkExtract(Word, 2, 4));
  EXPECT_TRUE(
      hasUnconstrainedProjectionInput(Ctx, Predicate, Overlap, 32, 100));
  EXPECT_FALSE(
      hasUnconstrainedProjectionInput(Ctx, Predicate, Overlap, 64, 100));
  const SymRef Repeated = Ctx.mkConcat(Ctx.mkExtract(Word, 0, 4),
                                       Ctx.mkNot(Ctx.mkExtract(Word, 0, 4)));
  EXPECT_FALSE(
      hasUnconstrainedProjectionInput(Ctx, Predicate, Repeated, 16, 100));
  EXPECT_TRUE(
      hasUnconstrainedProjectionInput(Ctx, Predicate, Repeated, 15, 100));
}

TEST(FiniteValues, SignExtensionDoesNotMultiplyTheSignBitDomain) {
  SymContext Ctx;
  const SymRef Predicate = Ctx.mkTrue();
  const SymRef Boolean = Ctx.mkVar("sign", 1);
  const SymRef Signed = Ctx.mkSExt(Boolean, 64);
  EXPECT_TRUE(hasUnconstrainedProjectionInput(Ctx, Predicate, Signed, 1, 100));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate, Signed, 2, 100));
  const SymRef Narrow = Ctx.mkVar("narrow", 3);
  for (SymRef Extended : {Ctx.mkZExt(Narrow, 64), Ctx.mkSExt(Narrow, 64)}) {
    EXPECT_TRUE(
        hasUnconstrainedProjectionInput(Ctx, Predicate, Extended, 7, 100));
    EXPECT_FALSE(
        hasUnconstrainedProjectionInput(Ctx, Predicate, Extended, 8, 100));
  }
}

TEST(FiniteValues, PredicateBitUsePreservesDisjointViews) {
  SymContext Ctx;
  const SymRef Word = Ctx.mkVar("word", 64);
  const SymRef Low = Ctx.mkExtract(Word, 0, 8);
  const SymRef Predicate =
      Ctx.mkEq(Ctx.mkExtract(Word, 63, 1), Ctx.mkConst(1, 0));
  EXPECT_TRUE(hasUnconstrainedProjectionInput(Ctx, Predicate, Low, 32, 100));
  const SymRef Independent = Ctx.mkVar("independent", 6);
  // A constrained or unsupported part does not invalidate distinct, proven
  // independent bits elsewhere in the same output.
  EXPECT_TRUE(hasUnconstrainedProjectionInput(
      Ctx, Predicate, Ctx.mkConcat(Low, Independent), 32, 100));
  EXPECT_TRUE(hasUnconstrainedProjectionInput(
      Ctx, Predicate,
      Ctx.mkConcat(Ctx.mkAdd(Low, Ctx.mkConst(8, 1)), Independent), 32, 100));
}

TEST(FiniteValues, TwoVaryingBitwiseOperandsDoNotProveIndependentBits) {
  SymContext Ctx;
  const SymRef A = Ctx.mkVar("a", 8);
  const SymRef B = Ctx.mkVar("b", 8);
  for (SymRef Value : {Ctx.mkAnd(A, B), Ctx.mkOr(A, B), Ctx.mkXor(A, B)})
    EXPECT_FALSE(
        hasUnconstrainedProjectionInput(Ctx, Ctx.mkTrue(), Value, 32, 100));
}

TEST(FiniteValues, SharedBudgetIncludesEveryRequiredOutputBitProof) {
  SymContext Ctx;
  const SymRef Word = Ctx.mkVar("word", 64);
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Ctx.mkTrue(), Word, 32, 6));
  EXPECT_TRUE(hasUnconstrainedProjectionInput(Ctx, Ctx.mkTrue(), Word, 32, 7));
  // A wide source is safe to inspect through a small view, but a whole wide
  // output is outside this recovery-only shortcut's <=64-bit contract.
  const SymRef Wide = Ctx.mkVar("wide", 4096);
  EXPECT_FALSE(
      hasUnconstrainedProjectionInput(Ctx, Ctx.mkTrue(), Wide, 32, 100));
  EXPECT_TRUE(hasUnconstrainedProjectionInput(
      Ctx, Ctx.mkTrue(), Ctx.mkExtract(Wide, 2048, 8), 32, 100));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(
      Ctx, Ctx.mkTrue(), SymRef(Ctx.numNodes() + 1), 32, 100));
}

TEST(FiniteValues, BitProjectionLowerBoundAgreesWithExhaustiveEnumeration) {
  SymContext Ctx;
  const SymRef A = Ctx.mkVar("a", 6);
  const SymRef B = Ctx.mkVar("b", 3);
  const SymRef Selector = Ctx.mkVar("selector", 2);
  const SymRef Predicate = Ctx.mkEq(Selector, Ctx.mkConst(2, 1));
  const SymRef Values[] = {
      Ctx.mkAnd(A, Ctx.mkConst(6, 0x3a)),
      Ctx.mkOr(A, Ctx.mkConst(6, 0x12)),
      Ctx.mkNot(A),
      Ctx.mkSExt(B, 8),
      Ctx.mkConcat(Ctx.mkExtract(A, 0, 3), B),
      Ctx.mkConcat(Ctx.mkExtract(A, 0, 3), Ctx.mkExtract(A, 2, 3))};
  for (SymRef Value : Values)
    for (uint32_t Limit : {1u, 3u, 7u, 15u, 31u, 63u}) {
      uint64_t Queries = 0;
      const auto Domain =
          enumerateFiniteValues(Ctx, Predicate, {Value}, Limit, {}, Queries);
      ASSERT_TRUE(Domain.Status == FiniteValueStatus::Complete ||
                  Domain.Status == FiniteValueStatus::TooManyValues);
      EXPECT_EQ(
          hasUnconstrainedProjectionInput(Ctx, Predicate, Value, Limit, 1000),
          Domain.Status == FiniteValueStatus::TooManyValues);
    }
}

TEST(FiniteValues, RelatedColumnsExcludeEveryDemandedSourceBit) {
  SymContext Ctx;
  const SymRef Word = Ctx.mkVar("word", 64);
  const SymRef Other = Ctx.mkVar("other", 64);
  const SymRef Value = Ctx.mkAnd(Word, Ctx.mkConst(64, 3));
  const SymRef Independent = Ctx.mkAnd(Other, Ctx.mkConst(64, 15));
  const SymRef Predicate = Ctx.mkTrue();
  ASSERT_TRUE(hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 3, 100));
  EXPECT_TRUE(hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 3, 100,
                                              {Independent}));
  for (SymRef Related :
       {Value, Ctx.mkAnd(Word, Ctx.mkConst(64, 1)),
        Ctx.mkAdd(Word, Ctx.mkConst(64, 1)),
        Ctx.mkConcat(Ctx.mkExtract(Word, 0, 1), Ctx.mkExtract(Other, 0, 1))}) {
    const size_t Nodes = Ctx.numNodes();
    EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 3, 100,
                                                 {Related}));
    EXPECT_EQ(Ctx.numNodes(), Nodes);
  }
  EXPECT_TRUE(hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 3, 100,
                                              {Ctx.mkExtract(Word, 63, 1)}));
  EXPECT_TRUE(hasUnconstrainedProjectionInput(
      Ctx, Ctx.mkEq(Ctx.mkExtract(Word, 63, 1), Ctx.mkConst(1, 0)), Value, 3,
      100, {Independent}));
}

TEST(FiniteValues, PartialInputProofKeepsCarriesAndEveryRelatedRoot) {
  SymContext Ctx;
  const auto Word = Ctx.mkVar("word", 4);
  const auto Predicate =
      Ctx.mkEq(Ctx.mkAnd(Word, Ctx.mkConst(4, 12)), Ctx.mkConst(4, 0));
  const auto Value = Ctx.mkAnd(Word, Ctx.mkConst(4, 3));
  const auto Carry = Ctx.mkExtract(Ctx.mkAdd(Word, Ctx.mkConst(4, 1)), 2, 1);
  EXPECT_FALSE(
      hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 3, 1000, {Carry}));
  // In the fixed Related=1 fiber only word=3 remains; a carry cannot be
  // discarded merely because the related expression extracts an upper bit.
  std::set<unsigned> CarryFiber;
  for (unsigned X = 0; X < 16; ++X)
    if (!(X & 12) && (((X + 1) >> 2) & 1))
      CarryFiber.insert(X & 3);
  EXPECT_EQ(CarryFiber, (std::set<unsigned>{3}));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(
      Ctx, Predicate, Value, 1, 1000,
      {Ctx.mkExtract(Word, 0, 1), Ctx.mkExtract(Word, 1, 1)}));
  const auto Zero = Ctx.mkEq(Word, Ctx.mkConst(4, 0));
  const auto One = Ctx.mkNot(Zero);
  ASSERT_TRUE(Ctx.isConstZero(Ctx.mkAnd(Zero, One)));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 3, 1000,
                                               {Zero, One}));
}

TEST(FiniteValues, PartialInputLowerBoundMatchesIndependentFibers) {
  SymContext Ctx;
  const auto A = Ctx.mkVar("a", 6);
  const auto B = Ctx.mkVar("b", 3);
  const auto LowA = Ctx.mkExtract(A, 0, 2);
  const auto LowB = Ctx.mkExtract(B, 0, 1);
  for (unsigned Mode = 0; Mode < 6; ++Mode) {
    SCOPED_TRACE(Mode);
    auto Predicate = Ctx.mkEq(LowA, Ctx.mkConst(2, 1));
    auto Value = Ctx.mkConcat(Ctx.mkExtract(A, 2, 4), B);
    llvm::SmallVector<SymRef, 4> Related{LowA, LowB};
    if (Mode == 1)
      Related.push_back(Ctx.mkExtract(A, 5, 1));
    if (Mode == 2) {
      Related.push_back(Ctx.mkEq(A, Ctx.mkConst(6, 1)));
      Related.push_back(Ctx.mkEq(A, Ctx.mkConst(6, 5)));
    }
    const auto Carry = Ctx.mkExtract(Ctx.mkAdd(A, Ctx.mkConst(6, 1)), 2, 1);
    if (Mode == 3)
      Related.push_back(Carry);
    if (Mode == 4)
      Predicate = Ctx.mkAnd(Predicate, Ctx.mkEq(Carry, Ctx.mkConst(1, 0)));
    if (Mode == 5)
      Value = Ctx.mkSExt(B, 64);

    // Independent host arithmetic enumerates every concrete input, grouped
    // by the exact related outputs that must remain fixed during variation.
    std::map<std::vector<uint64_t>, std::set<uint64_t>> Fibers;
    for (unsigned X = 0; X < 64; ++X)
      for (unsigned Y = 0; Y < 8; ++Y) {
        if ((X & 3) != 1 || (Mode == 4 && (((X + 1) >> 2) & 1)))
          continue;
        std::vector<uint64_t> Key{X & 3U, Y & 1U};
        if (Mode == 1)
          Key.push_back(X >> 5);
        if (Mode == 2) {
          Key.push_back(X == 1);
          Key.push_back(X == 5);
        }
        if (Mode == 3)
          Key.push_back(((X + 1) >> 2) & 1);
        const uint64_t Output = Mode == 5
                                    ? uint64_t((Y & 4) ? int64_t(Y) - 8 : Y)
                                    : ((X >> 2) << 3) | Y;
        Fibers[Key].insert(Output);
      }
    ASSERT_FALSE(Fibers.empty());
    size_t Smallest = std::numeric_limits<size_t>::max();
    for (const auto &[Key, Values] : Fibers)
      Smallest = std::min(Smallest, Values.size());
    const auto Nodes = Ctx.numNodes();
    for (uint32_t Limit : {1U, 3U, 7U, 15U, 31U, 63U, 127U})
      EXPECT_EQ(hasUnconstrainedProjectionInput(Ctx, Predicate, Value, Limit,
                                                5000, Related),
                Smallest > Limit)
          << "limit=" << Limit << " smallest fiber=" << Smallest;
    EXPECT_EQ(Ctx.numNodes(), Nodes);
  }
}

TEST(FiniteValues, PartialInputProofSharesBudgetAndPreservesWideFallback) {
  SymContext Ctx;
  const auto Word = Ctx.mkVar("word", 64);
  const auto Predicate = Ctx.mkEq(Ctx.mkExtract(Word, 0, 1), Ctx.mkConst(1, 1));
  const auto Value = Ctx.mkExtract(Word, 8, 8);
  const auto Related = Ctx.mkExtract(Word, 1, 1);
  uint64_t Minimum = 0;
  for (uint64_t Budget = 1; Budget < 1000; ++Budget)
    if (hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 255, Budget,
                                        {Related})) {
      Minimum = Budget;
      break;
    }
  ASSERT_GT(Minimum, 1U);
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 255,
                                               Minimum - 1, {Related}));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 255,
                                               Minimum, {Related, Related}));
  EXPECT_TRUE(hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 255, 1000,
                                              {Related, Related}));
  const auto Wide = Ctx.mkVar("wide", 4096);
  const auto WideView = Ctx.mkExtract(Wide, 2048, 8);
  EXPECT_TRUE(
      hasUnconstrainedProjectionInput(Ctx, Ctx.mkTrue(), WideView, 255, 1000));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(
      Ctx, Ctx.mkEq(Ctx.mkExtract(Wide, 0, 1), Ctx.mkConst(1, 1)), WideView,
      255, 1000));
}

TEST(FiniteValues, PartialInputProofChargesRepeatedRelatedOperandScans) {
  SymContext Ctx;
  const auto Word = Ctx.mkVar("word", 64);
  const auto Payload = Ctx.mkExtract(Word, 8, 8);
  const auto Predicate = Ctx.mkEq(Ctx.mkExtract(Word, 1, 1), Ctx.mkConst(1, 1));
  llvm::SmallVector<SymRef, 8> Parts(2048, Ctx.mkExtract(Word, 0, 1));
  const auto WideRelated = Ctx.mkConcat(Parts);
  llvm::SmallVector<SymRef, 8> Related(8, WideRelated);
  // The initial whole-DAG scan sees the wide node once. Fine-grained walks
  // must charge its operand edges again for each related root, even though
  // all 2048 pieces denote the very same source bit.
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate, Payload, 255,
                                               6000, Related));
  EXPECT_TRUE(hasUnconstrainedProjectionInput(Ctx, Predicate, Payload, 255,
                                              50000, Related));
}

TEST(FiniteValues, RelatedRootsShareTheTraversalBudgetAndMustBeValid) {
  SymContext Ctx;
  const SymRef Word = Ctx.mkVar("word", 64);
  const SymRef Other = Ctx.mkVar("other", 64);
  const SymRef Predicate = Ctx.mkTrue();
  EXPECT_FALSE(
      hasUnconstrainedProjectionInput(Ctx, Predicate, Word, 32, 7, {Other}));
  EXPECT_TRUE(
      hasUnconstrainedProjectionInput(Ctx, Predicate, Word, 32, 8, {Other}));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate, Word, 32, 8,
                                               {Other, Other}));
  EXPECT_TRUE(hasUnconstrainedProjectionInput(Ctx, Predicate, Word, 32, 9,
                                              {Other, Other}));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate, Word, 32, 100,
                                               {SymRef{}}));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate, Word, 32, 100,
                                               {SymRef(Ctx.numNodes() + 1)}));
}

TEST(FiniteValues, CompleteMaskedColumnsNeedIndependentCartesianFactors) {
  SymContext Ctx;
  const SymRef A = Ctx.mkVar("a", 2);
  const SymRef B = Ctx.mkVar("b", 2);
  const SymRef Selector = Ctx.mkVar("selector", 1);
  const SymRef Predicate = Ctx.mkEq(Selector, Ctx.mkConst(1, 1));
  const SymRef Candidate = Ctx.mkZExt(A, 8);
  uint64_t Queries = 0;
  const auto CandidateDomain =
      enumerateFiniteValues(Ctx, Predicate, {Candidate}, 16, {}, Queries);
  ASSERT_EQ(CandidateDomain.Status, FiniteValueStatus::Complete);
  ASSERT_EQ(CandidateDomain.Tuples.size(), 4u);
  for (bool Correlated : {false, true}) {
    const SymRef Related = Correlated ? Ctx.mkAnd(A, Ctx.mkConst(2, 1)) : B;
    const auto Kept =
        enumerateFiniteValues(Ctx, Predicate, {Related}, 16, {}, Queries);
    const auto Joint = enumerateFiniteValues(
        Ctx, Predicate, {Candidate, Related}, 16, {}, Queries);
    ASSERT_EQ(Kept.Status, FiniteValueStatus::Complete);
    ASSERT_EQ(Joint.Status, FiniteValueStatus::Complete);
    const bool Independent = hasUnconstrainedProjectionInput(
        Ctx, Predicate, Candidate, 3, 100, {Related});
    EXPECT_EQ(Independent, !Correlated);
    if (Independent) {
      EXPECT_EQ(Joint.Tuples.size(),
                CandidateDomain.Tuples.size() * Kept.Tuples.size());
      for (const auto &Column : CandidateDomain.Tuples)
        for (const auto &Rest : Kept.Tuples)
          EXPECT_NE(std::find(Joint.Tuples.begin(), Joint.Tuples.end(),
                              std::vector<uint64_t>{Column[0], Rest[0]}),
                    Joint.Tuples.end());
    } else {
      // A full four-value column can still correlate with another column.
      // Dropping it as TOP would add four impossible joint tuples here.
      EXPECT_LT(Joint.Tuples.size(),
                CandidateDomain.Tuples.size() * Kept.Tuples.size());
    }
  }
  const SymRef Partial = Ctx.mkAnd(Candidate, Ctx.mkConst(8, 1));
  EXPECT_FALSE(
      hasUnconstrainedProjectionInput(Ctx, Predicate, Partial, 3, 100, {B}));
}

TEST(FiniteValues, PristineDomainCopiesKeepTuplesObserversAndQueryCharges) {
  SymContext Ctx;
  const auto X = Ctx.mkVar("x", 4), Y = Ctx.mkVar("y", 4);
  const auto Predicate = Ctx.mkEq(Ctx.mkAdd(X, Y), Ctx.mkConst(4, 7));
  neverd::solver::SolverOptions Settings;
  Settings.BuildModel = false;
  FiniteDomainEncoding Encoding(Ctx, Settings);
  for (unsigned Run = 0; Run != 12; ++Run) {
    const std::vector<SymRef> Values =
        Run % 2 ? std::vector<SymRef>{Y, X}
                : std::vector<SymRef>{X, Ctx.mkAdd(X, Y)};
    uint64_t FreshQueries = 0, CopyQueries = 0;
    std::vector<std::vector<uint64_t>> FreshSeen, CopySeen;
    const auto Limit = Run % 3 == 0 ? 16U : 17U;
    const bool Abandon = Run % 4 == 0;
    const auto Fresh = enumerateFiniteValues(
        Ctx, Predicate, Values, 16, Settings, Limit, 100000, FreshQueries,
        [&](llvm::ArrayRef<uint64_t> Tuple) {
          FreshSeen.emplace_back(Tuple.begin(), Tuple.end());
          return !Abandon;
        });
    const auto Copy = enumerateFiniteValues(
        Encoding, Predicate, Values, 16, Limit, 100000, CopyQueries,
        [&](llvm::ArrayRef<uint64_t> Tuple) {
          CopySeen.emplace_back(Tuple.begin(), Tuple.end());
          return !Abandon;
        });
    EXPECT_EQ(Copy.Status, Fresh.Status);
    EXPECT_EQ(Copy.Tuples, Fresh.Tuples);
    EXPECT_EQ(CopySeen, FreshSeen);
    EXPECT_EQ(CopyQueries, FreshQueries);
    if (!Abandon && Limit == 17) {
      EXPECT_EQ(Copy.Status, FiniteValueStatus::Complete);
      ASSERT_EQ(Copy.Tuples.size(), 16U);
      for (unsigned J = 0; J != 16; ++J) {
        EXPECT_EQ(Copy.Tuples[J][0], J);
        EXPECT_EQ(Copy.Tuples[J][1], Run % 2 ? ((7U - J) & 15U) : 7U);
      }
      EXPECT_EQ(CopyQueries, 17U);
    } else {
      EXPECT_TRUE(Copy.Tuples.empty());
      EXPECT_EQ(Copy.Status, Abandon ? FiniteValueStatus::Unknown
                                     : FiniteValueStatus::QueryBudgetExceeded);
    }
  }
}

TEST(FiniteValues, DomainReplacementAndSeparateContextsRetainExactPredicates) {
  for (unsigned Bias : {0U, 1U}) {
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", 8);
    FiniteDomainEncoding Encoding(Ctx, {});
    for (unsigned Value : {5U, 9U, 5U, 13U}) {
      auto Predicate = Ctx.mkEq(X, Ctx.mkConst(8, Value + Bias));
      uint64_t Queries = 0;
      const auto Result =
          enumerateFiniteValues(Encoding, Predicate, {X}, 1, 2, 10000, Queries);
      EXPECT_EQ(Result.Status, FiniteValueStatus::Complete);
      EXPECT_EQ(Result.Tuples,
                (std::vector<std::vector<uint64_t>>{{Value + Bias}}));
      EXPECT_EQ(Queries, 2U);
      Queries = 0;
      const auto Limited =
          enumerateFiniteValues(Encoding, Predicate, {X}, 1, 1, 10000, Queries);
      EXPECT_EQ(Limited.Status, FiniteValueStatus::QueryBudgetExceeded);
      EXPECT_TRUE(Limited.Tuples.empty());
      EXPECT_EQ(Queries, 1U);
    }
  }
}

TEST(FiniteValues, FailedProjectionNeverPoisonsThePristineDomain) {
  SymContext Ctx;
  const auto X = Ctx.mkVar("x", 8), Y = Ctx.mkVar("y", 8);
  const auto Z = Ctx.mkVar("z", 8);
  const auto Predicate = Ctx.mkEq(X, Ctx.mkConst(8, 5));
  // Both factors remain free after asserting the predicate, so constant
  // folding cannot remove the deliberately oversized projection circuit.
  const auto Product = Ctx.mkMul(Y, Z);
  neverd::solver::SolverOptions Settings;
  Settings.Blast.MaxGates = 64;
  FiniteDomainEncoding Encoding(Ctx, Settings);
  for (SymRef Value : {Product, X, Product, X}) {
    uint64_t FreshQueries = 0, CopyQueries = 0;
    const auto Fresh = enumerateFiniteValues(Ctx, Predicate, {Value}, 1,
                                             Settings, 2, 10000, FreshQueries);
    const auto Copy = enumerateFiniteValues(Encoding, Predicate, {Value}, 1, 2,
                                            10000, CopyQueries);
    EXPECT_EQ(Copy.Status, Fresh.Status);
    EXPECT_EQ(Copy.Tuples, Fresh.Tuples);
    EXPECT_EQ(CopyQueries, FreshQueries);
    if (Value == X) {
      EXPECT_EQ(Copy.Status, FiniteValueStatus::Complete);
      EXPECT_EQ(Copy.Tuples, (std::vector<std::vector<uint64_t>>{{5}}));
    } else {
      EXPECT_EQ(Copy.Status, FiniteValueStatus::Unknown);
      EXPECT_TRUE(Copy.Tuples.empty());
      EXPECT_EQ(CopyQueries, 0U);
    }
  }
}

TEST(FiniteValues, EncodingFailureIsExactAndResetOnEveryOutcome) {
  using namespace neverd::solver;
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  auto Domain = C.mkEq(C.mkMul(X, Y), C.mkConst(8, 3));
  SolverOptions Tiny;
  Tiny.Blast.MaxGates = 1;
  FiniteDomainEncoding Gates(C, Tiny);
  uint64_t Queries = 0;
  auto Error = BlastError::Malformed;
  auto R =
      enumerateFiniteValues(Gates, Domain, {X}, 8, 100, 10000, Queries, Error);
  EXPECT_EQ(R.Status, FiniteValueStatus::Unknown);
  EXPECT_EQ(Error, BlastError::TooManyGates);
  EXPECT_TRUE(R.Tuples.empty());
  EXPECT_EQ(Queries, 0U);
  // Early outcomes must not retain the previous encoder's diagnostic.
  R = enumerateFiniteValues(Gates, {}, {X}, 8, 100, 10000, Queries, Error);
  EXPECT_EQ(R.Status, FiniteValueStatus::Invalid);
  EXPECT_EQ(Error, BlastError::None);
  Error = BlastError::TooManyGates;
  R = enumerateFiniteValues(Gates, C.mkFalse(), {X}, 8, 100, 10000, Queries,
                            Error);
  EXPECT_EQ(R.Status, FiniteValueStatus::Complete);
  EXPECT_EQ(Error, BlastError::None);
  Error = BlastError::TooManyGates;
  R = enumerateFiniteValues(Gates, C.mkTrue(), {X}, 1, 100, 10000, Queries,
                            Error);
  EXPECT_EQ(R.Status, FiniteValueStatus::TooManyValues);
  EXPECT_EQ(Error, BlastError::None);
  SolverOptions Narrow;
  Narrow.Blast.MaxWidth = 4;
  FiniteDomainEncoding Width(C, Narrow);
  Error = BlastError::None;
  R = enumerateFiniteValues(Width, Domain, {X}, 8, 100, 10000, Queries, Error);
  EXPECT_EQ(R.Status, FiniteValueStatus::Unknown);
  EXPECT_EQ(Error, BlastError::WidthTooLarge);
  auto Malformed = C.mkEq(X, Y);
  auto &Node = const_cast<SymNode &>(C.node(Malformed));
  auto Op = Node.Op;
  Node.Op = SymOp::Ite;
  FiniteDomainEncoding Invalid(C, {});
  Error = BlastError::None;
  R = enumerateFiniteValues(Invalid, Malformed, {X}, 8, 100, 10000, Queries,
                            Error);
  Node.Op = Op;
  EXPECT_EQ(R.Status, FiniteValueStatus::Invalid);
  EXPECT_EQ(Error, BlastError::Malformed);
}
TEST(FiniteValues, SearchAndGlobalRefusalsAreNotEncodingFailures) {
  using namespace neverd::solver;
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  auto Domain = C.mkEq(C.mkAdd(X, C.mkConst(8, 1)), Y);
  SolverOptions Search;
  Search.Sat.MaxPropagations = 1;
  FiniteDomainEncoding Limited(C, Search);
  uint64_t Queries = 0;
  auto Error = BlastError::TooManyGates;
  auto R = enumerateFiniteValues(Limited, Domain, {X}, 256, 1000, 10000,
                                 Queries, Error);
  EXPECT_EQ(R.Status, FiniteValueStatus::Unknown);
  EXPECT_EQ(Error, BlastError::None);
  FiniteDomainEncoding Full(C, {});
  Queries = 0;
  Error = BlastError::TooManyGates;
  auto Point = C.mkEq(X, C.mkConst(8, 7));
  R = enumerateFiniteValues(Full, Point, {X}, 1, 0, 10000, Queries, Error);
  EXPECT_EQ(R.Status, FiniteValueStatus::QueryBudgetExceeded);
  EXPECT_EQ(Error, BlastError::None);
  Error = BlastError::TooManyGates;
  R = enumerateFiniteValues(Full, Point, {X}, 1, 2, 10000, Queries, Error);
  EXPECT_EQ(R.Status, FiniteValueStatus::Complete);
  EXPECT_EQ(Error, BlastError::None);
  EXPECT_EQ(Queries, 2U);
  EXPECT_EQ(R.Tuples, (std::vector<std::vector<uint64_t>>{{7}}));
  Queries = 0;
  Error = BlastError::TooManyGates;
  R = enumerateFiniteValues(Full, Domain, {X}, 1, 1000, 10000, Queries, Error);
  EXPECT_EQ(R.Status, FiniteValueStatus::TooManyValues);
  EXPECT_EQ(Error, BlastError::None);
  EXPECT_TRUE(R.Tuples.empty());
}
} // namespace
