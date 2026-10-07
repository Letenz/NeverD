//===- FiniteQueryCacheTests.cpp - Exact finite-proof cache keys ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/analysis/core/FiniteQueryCache.h"
#include "../../lib/analysis/core/FrameOffsets.h"
#include "gtest/gtest.h"

#include "neverd/symbolic/SymState.h"

using namespace neverd::analysis;
using namespace neverd::analysis::detail;
using namespace neverd::symbolic;

namespace {
FiniteValues prove(SymContext &Ctx, SymRef Predicate,
                   llvm::ArrayRef<SymRef> Values, uint32_t Limit) {
  uint64_t Queries = 0;
  return enumerateFiniteValues(Ctx, Predicate, Values, Limit,
                               SpecializationOptions{}, Queries);
}

void expectHit(const std::optional<FiniteValues> &Hit,
               const FiniteValues &Expected) {
  ASSERT_TRUE(Hit);
  EXPECT_EQ(Hit->Status, Expected.Status);
  EXPECT_EQ(Hit->Tuples, Expected.Tuples);
}

TEST(FiniteQueryCache, AlphaRenamingIgnoresContextAndInputMetadata) {
  FiniteQueryCache Cache(4096);
  SymContext First;
  const auto X = First.mkVar("source", 8);
  const auto P = First.mkUle(X, First.mkConst(8, 3));
  const SymRef Values[] = {X, First.mkAnd(X, First.mkConst(8, 1))};
  const auto Result = prove(First, P, Values, 8);
  ASSERT_EQ(Result.Status, FiniteValueStatus::Complete);
  ASSERT_EQ(Result.Tuples.size(), 4u);
  Cache.store(First, P, Values, 8, Result);

  SymContext Second;
  Second.mkVar("unrelated", 64);
  Second.mkFreshVar(16);
  const auto Y =
      Second.mkInputVar("renamed", 8, {SymInputKind::Register, 32, 1, 2});
  const auto Q = Second.mkUle(Y, Second.mkConst(8, 3));
  const SymRef Renamed[] = {Y, Second.mkAnd(Y, Second.mkConst(8, 1))};
  ASSERT_NE(First.varId(X), Second.varId(Y));
  const auto Nodes = Second.numNodes();
  expectHit(Cache.lookup(Second, Q, Renamed, 8), Result);
  EXPECT_EQ(Second.numNodes(), Nodes);
  const auto Independent = prove(Second, Q, Renamed, 8);
  EXPECT_EQ(Independent.Tuples, Result.Tuples);
}

TEST(FiniteQueryCache, LargeSparseDagsKeepRenamingAndSharedRoots) {
  struct Query {
    SymRef Predicate;
    std::vector<SymRef> Values;
    SymRef Other;
  };
  const auto Build = [](SymContext &Ctx, bool Sparse) {
    std::vector<SymRef> Variables, Conditions;
    for (unsigned I = 0; I != 512; ++I) {
      if (Sparse)
        for (unsigned J = 0; J != 7; ++J)
          Ctx.mkFreshVar(64, "unrelated");
      const auto X = Ctx.mkFreshVar(8, Sparse ? "renamed" : "source");
      Variables.push_back(X);
      Conditions.push_back(Ctx.mkEq(X, Ctx.mkConst(8, I % 251)));
    }
    return Query{Ctx.mkAnd(Conditions),
                 {Variables[17], Variables[233],
                  Ctx.mkConcat(Variables[17], Variables[233])},
                 Variables[18]};
  };
  SymContext First, Second;
  const auto A = Build(First, false), B = Build(Second, true);
  ASSERT_GT(Second.numNodes(), First.numNodes() + 3000);
  const auto Expected = prove(First, A.Predicate, A.Values, 2);
  ASSERT_EQ(Expected.Status, FiniteValueStatus::Complete);
  ASSERT_EQ(Expected.Tuples,
            (std::vector<std::vector<uint64_t>>{{17, 233, 0x11e9}}));
  FiniteQueryCache Cache(65536);
  Cache.store(First, A.Predicate, A.Values, 2, Expected);
  expectHit(Cache.lookup(Second, B.Predicate, B.Values, 2), Expected);
  EXPECT_EQ(prove(Second, B.Predicate, B.Values, 2).Tuples, Expected.Tuples);
  // The first two roots are also operands of the third root. Replacing only
  // one with a different predicate variable must not reuse that proof.
  auto Changed = B.Values;
  Changed[0] = B.Other;
  EXPECT_FALSE(Cache.lookup(Second, B.Predicate, Changed, 2));
  EXPECT_FALSE(Cache.lookup(Second, B.Predicate, B.Values, 1));
  expectHit(Cache.lookup(First, A.Predicate, A.Values, 2), Expected);
}

TEST(FiniteQueryCache, SharedAndIndependentVariablesCannotShareAKey) {
  FiniteQueryCache Cache(4096);
  SymContext Ctx;
  const auto X = Ctx.mkVar("x", 1);
  const auto Y = Ctx.mkVar("y", 1);
  const auto P = Ctx.mkTrue();
  const SymRef Shared[] = {X, X};
  const SymRef Independent[] = {X, Y};
  const auto SharedResult = prove(Ctx, P, Shared, 4);
  ASSERT_EQ(SharedResult.Status, FiniteValueStatus::Complete);
  ASSERT_EQ(SharedResult.Tuples.size(), 2u);
  Cache.store(Ctx, P, Shared, 4, SharedResult);
  EXPECT_FALSE(Cache.lookup(Ctx, P, Independent, 4));
  const auto IndependentResult = prove(Ctx, P, Independent, 4);
  ASSERT_EQ(IndependentResult.Tuples.size(), 4u);
  Cache.store(Ctx, P, Independent, 4, IndependentResult);
  expectHit(Cache.lookup(Ctx, P, Shared, 4), SharedResult);
  expectHit(Cache.lookup(Ctx, P, Independent, 4), IndependentResult);
}

TEST(FiniteQueryCache, PredicateAndValuesShareOneVariableRenaming) {
  FiniteQueryCache Cache(4096);
  SymContext Ctx;
  const auto X = Ctx.mkVar("x", 8);
  const auto Y = Ctx.mkVar("y", 8);
  const auto P = Ctx.mkEq(X, Ctx.mkConst(8, 19));
  const auto Result = prove(Ctx, P, {X}, 4);
  ASSERT_EQ(Result.Tuples, (std::vector<std::vector<uint64_t>>{{19}}));
  Cache.store(Ctx, P, {X}, 4, Result);
  EXPECT_FALSE(Cache.lookup(Ctx, P, {Y}, 4));
  EXPECT_EQ(prove(Ctx, P, {Y}, 4).Status, FiniteValueStatus::TooManyValues);
}

TEST(FiniteQueryCache, WidthConstantsAndEnumerationLimitArePartOfTheKey) {
  FiniteQueryCache Cache(4096);
  SymContext Ctx;
  const auto Value = Ctx.mkConst(8, 7);
  const auto P = Ctx.mkTrue();
  const auto Result = prove(Ctx, P, {Value}, 4);
  Cache.store(Ctx, P, {Value}, 4, Result);
  EXPECT_FALSE(Cache.lookup(Ctx, P, {Ctx.mkConst(16, 7)}, 4));
  EXPECT_FALSE(Cache.lookup(Ctx, P, {Ctx.mkConst(8, 8)}, 4));
  EXPECT_FALSE(Cache.lookup(Ctx, P, {Value}, 5));
  EXPECT_FALSE(Cache.lookup(Ctx, Ctx.mkFalse(), {Value}, 4));
  expectHit(Cache.lookup(Ctx, P, {Value}, 4), Result);
}

TEST(FiniteQueryCache, ExtractAuxAndOrderedOperandsRemainDistinct) {
  FiniteQueryCache Cache(4096);
  SymContext Ctx;
  const auto X = Ctx.mkVar("x", 8);
  const auto Y = Ctx.mkVar("y", 8);
  const auto P =
      Ctx.mkAnd(Ctx.mkEq(X, Ctx.mkConst(8, 6)), Ctx.mkEq(Y, Ctx.mkConst(8, 3)));
  const auto Low = Ctx.mkExtract(X, 0, 1);
  Cache.store(Ctx, P, {Low}, 4, prove(Ctx, P, {Low}, 4));
  EXPECT_FALSE(Cache.lookup(Ctx, P, {Ctx.mkExtract(X, 1, 1)}, 4));
  const auto Quotient = Ctx.mkUDiv(X, Y);
  const auto Result = prove(Ctx, P, {Quotient}, 4);
  ASSERT_EQ(Result.Tuples, (std::vector<std::vector<uint64_t>>{{2}}));
  Cache.store(Ctx, P, {Quotient}, 4, Result);
  EXPECT_FALSE(Cache.lookup(Ctx, P, {Ctx.mkUDiv(Y, X)}, 4));
  const auto Sum = Ctx.mkAdd(X, Y);
  Cache.store(Ctx, P, {Sum}, 4, prove(Ctx, P, {Sum}, 4));
  EXPECT_FALSE(Cache.lookup(Ctx, P, {Ctx.mkXor(X, Y)}, 4));
  Cache.store(Ctx, P, {X, Y}, 4, prove(Ctx, P, {X, Y}, 4));
  EXPECT_FALSE(Cache.lookup(Ctx, P, {Y, X}, 4));
}

TEST(FiniteQueryCache, WideConstantsUseTheirBitsInsteadOfPoolIndices) {
  FiniteQueryCache Cache(4096);
  llvm::APInt Constant(128, 23);
  Constant.setBit(91);
  SymContext First;
  const auto X = First.mkVar("wide", 128);
  const auto P = First.mkEq(X, First.mkConst(Constant));
  const auto Value = First.mkExtract(X, 0, 8);
  const auto Result = prove(First, P, {Value}, 4);
  ASSERT_EQ(Result.Status, FiniteValueStatus::Complete);
  Cache.store(First, P, {Value}, 4, Result);

  SymContext Second;
  Second.mkConst(llvm::APInt::getOneBitSet(128, 100));
  const auto Y = Second.mkFreshVar(128, "other");
  const auto Q = Second.mkEq(Y, Second.mkConst(Constant));
  const auto Other = Second.mkExtract(Y, 0, 8);
  expectHit(Cache.lookup(Second, Q, {Other}, 4), Result);
  Constant.flipBit(90);
  EXPECT_FALSE(Cache.lookup(Second, Second.mkEq(Y, Second.mkConst(Constant)),
                            {Other}, 4));
}

TEST(FiniteQueryCache, CompleteUnsatisfiableAndTooManyResultsAreReusable) {
  FiniteQueryCache Cache(4096);
  SymContext Ctx;
  const auto X = Ctx.mkVar("x", 8);
  const auto None = prove(Ctx, Ctx.mkFalse(), {X}, 2);
  ASSERT_EQ(None.Status, FiniteValueStatus::Complete);
  ASSERT_TRUE(None.Tuples.empty());
  Cache.store(Ctx, Ctx.mkFalse(), {X}, 2, None);
  expectHit(Cache.lookup(Ctx, Ctx.mkFalse(), {X}, 2), None);
  const auto Many = prove(Ctx, Ctx.mkTrue(), {X}, 2);
  ASSERT_EQ(Many.Status, FiniteValueStatus::TooManyValues);
  ASSERT_TRUE(Many.Tuples.empty());
  Cache.store(Ctx, Ctx.mkTrue(), {X}, 2, Many);
  expectHit(Cache.lookup(Ctx, Ctx.mkTrue(), {X}, 2), Many);
  EXPECT_FALSE(Cache.lookup(Ctx, Ctx.mkTrue(), {X}, 3));
}

TEST(FiniteQueryCache, IncompleteResultsAndMalformedTuplesAreNotRecorded) {
  SymContext Ctx;
  const auto X = Ctx.mkVar("x", 1);
  const auto P = Ctx.mkTrue();
  for (auto Status : {FiniteValueStatus::Unknown, FiniteValueStatus::Invalid,
                      FiniteValueStatus::QueryBudgetExceeded}) {
    FiniteQueryCache Cache(4096);
    Cache.store(Ctx, P, {X}, 2, {Status, {{0}}});
    EXPECT_FALSE(Cache.lookup(Ctx, P, {X}, 2));
  }
  for (const FiniteValues &Result :
       {FiniteValues{FiniteValueStatus::TooManyValues, {{0}}},
        FiniteValues{FiniteValueStatus::Complete, {{0, 1}}},
        FiniteValues{FiniteValueStatus::Complete, {{2}}},
        FiniteValues{FiniteValueStatus::Complete, {{0}, {1}, {0}}}}) {
    FiniteQueryCache Cache(4096);
    Cache.store(Ctx, P, {X}, 2, Result);
    EXPECT_FALSE(Cache.lookup(Ctx, P, {X}, 2));
  }
}

TEST(FiniteQueryCache, KeyAndResultStorageBudgetsOnlyCauseMisses) {
  SymContext Ctx;
  const auto P = Ctx.mkTrue();
  const auto Seven = Ctx.mkConst(8, 7);
  const auto Result = prove(Ctx, P, {Seven}, 2);
  for (uint64_t Budget : {0, 15, 32}) {
    FiniteQueryCache Cache(Budget);
    Cache.store(Ctx, P, {Seven}, 2, Result);
    EXPECT_FALSE(Cache.lookup(Ctx, P, {Seven}, 2));
  }
  FiniteQueryCache Exact(33);
  Exact.store(Ctx, P, {Seven}, 2, Result);
  expectHit(Exact.lookup(Ctx, P, {Seven}, 2), Result);
  const auto Eight = Ctx.mkConst(8, 8);
  const auto Other = prove(Ctx, P, {Eight}, 2);
  Exact.store(Ctx, P, {Eight}, 2, Other);
  expectHit(Exact.lookup(Ctx, P, {Eight}, 2), Other);
  EXPECT_FALSE(Exact.lookup(Ctx, P, {Seven}, 2));

  const auto X = Ctx.mkVar("bounded", 5);
  const auto Domain = prove(Ctx, P, {X}, 32);
  ASSERT_EQ(Domain.Status, FiniteValueStatus::Complete);
  ASSERT_EQ(Domain.Tuples.size(), 32u);
  FiniteQueryCache Small(64);
  Small.store(Ctx, P, {X}, 32, Domain);
  EXPECT_FALSE(Small.lookup(Ctx, P, {X}, 32));
  EXPECT_EQ(Domain.Tuples.size(), 32u);
}

TEST(FiniteQueryCache, RecentProofsReplaceColdEntriesAtCapacity) {
  FiniteQueryCache Cache(66);
  SymContext Ctx;
  const auto P = Ctx.mkTrue();
  const auto Seven = Ctx.mkConst(8, 7);
  const auto Eight = Ctx.mkConst(8, 8);
  const auto Nine = Ctx.mkConst(8, 9);
  const auto First = prove(Ctx, P, {Seven}, 2);
  const auto Second = prove(Ctx, P, {Eight}, 2);
  const auto Third = prove(Ctx, P, {Nine}, 2);
  Cache.store(Ctx, P, {Seven}, 2, First);
  Cache.store(Ctx, P, {Eight}, 2, Second);
  const FiniteQueryCache &ReadOnly = Cache;
  expectHit(ReadOnly.lookup(Ctx, P, {Seven}, 2), First);
  Cache.store(Ctx, P, {Nine}, 2, Third);
  EXPECT_FALSE(ReadOnly.lookup(Ctx, P, {Eight}, 2));
  expectHit(ReadOnly.lookup(Ctx, P, {Seven}, 2), First);
  expectHit(ReadOnly.lookup(Ctx, P, {Nine}, 2), Third);
  EXPECT_FALSE(ReadOnly.lookup(Ctx, P, {Eight}, 2));
  Cache.store(Ctx, P, {Eight}, 2, Second);
  EXPECT_FALSE(ReadOnly.lookup(Ctx, P, {Seven}, 2));
  expectHit(ReadOnly.lookup(Ctx, P, {Nine}, 2), Third);
  expectHit(ReadOnly.lookup(Ctx, P, {Eight}, 2), Second);
}

TEST(FiniteQueryCache, EvictedProofsCannotAliasRenamedReplacementKeys) {
  FiniteQueryCache Cache(64);
  SymContext Ctx;
  const auto X = Ctx.mkVar("source", 8);
  for (uint64_t Value = 1; Value != 97; ++Value) {
    const auto P = Ctx.mkEq(X, Ctx.mkConst(8, Value));
    const auto Result = prove(Ctx, P, {X}, 2);
    ASSERT_EQ(Result.Status, FiniteValueStatus::Complete);
    Cache.store(Ctx, P, {X}, 2, Result);
    if (Value > 1)
      EXPECT_FALSE(
          Cache.lookup(Ctx, Ctx.mkEq(X, Ctx.mkConst(8, Value - 1)), {X}, 2));
    SymContext Renamed;
    Renamed.mkVar("unrelated", 64);
    const auto Y = Renamed.mkFreshVar(8);
    const auto Q = Renamed.mkEq(Y, Renamed.mkConst(8, Value));
    expectHit(Cache.lookup(Renamed, Q, {Y}, 2), Result);
    EXPECT_EQ(Result.Tuples, (std::vector<std::vector<uint64_t>>{{Value}}));
  }
}

TEST(FiniteQueryCache, IneligibleAdmissionsPreserveRecordsAndRecency) {
  FiniteQueryCache Cache(66);
  SymContext Ctx;
  const auto P = Ctx.mkTrue();
  const auto Seven = Ctx.mkConst(8, 7);
  const auto Eight = Ctx.mkConst(8, 8);
  const auto Nine = Ctx.mkConst(8, 9);
  Cache.store(Ctx, P, {Seven}, 2, prove(Ctx, P, {Seven}, 2));
  Cache.store(Ctx, P, {Eight}, 2, prove(Ctx, P, {Eight}, 2));
  Cache.store(Ctx, P, {Seven}, 2, prove(Ctx, P, {Seven}, 2));
  const auto X = Ctx.mkVar("wide_domain", 5);
  const auto Large = prove(Ctx, P, {X}, 32);
  ASSERT_EQ(Large.Status, FiniteValueStatus::Complete);
  ASSERT_EQ(Large.Tuples.size(), 32u);
  Cache.store(Ctx, P, {X}, 32, Large);
  for (const FiniteValues &Invalid :
       {FiniteValues{FiniteValueStatus::Unknown, {}},
        FiniteValues{FiniteValueStatus::QueryBudgetExceeded, {}},
        FiniteValues{FiniteValueStatus::TooManyValues, {{7}}},
        FiniteValues{FiniteValueStatus::Complete, {{7, 8}}}}) {
    Cache.store(Ctx, P, {Nine}, 2, Invalid);
    Cache.store(Ctx, P, {Seven}, 2, Invalid);
  }
  Cache.store(Ctx, {}, {Nine}, 2, prove(Ctx, P, {Nine}, 2));
  const auto Third = prove(Ctx, P, {Nine}, 2);
  Cache.store(Ctx, P, {Nine}, 2, Third);
  EXPECT_FALSE(Cache.lookup(Ctx, P, {Seven}, 2));
  expectHit(Cache.lookup(Ctx, P, {Eight}, 2), prove(Ctx, P, {Eight}, 2));
  expectHit(Cache.lookup(Ctx, P, {Nine}, 2), Third);
}

TEST(FiniteQueryCache, MultipleEvictionsPreserveReturnedProofCopies) {
  FiniteQueryCache Cache(165);
  SymContext Ctx;
  const auto P = Ctx.mkTrue();
  const SymRef Values[] = {Ctx.mkConst(8, 7), Ctx.mkVar("two_bits", 2),
                           Ctx.mkVar("one_bit", 1)};
  for (auto Value : Values)
    Cache.store(Ctx, P, {Value}, 4, prove(Ctx, P, {Value}, 4));
  const auto X = Ctx.mkVar("five_bits", 5);
  const auto Large = prove(Ctx, P, {X}, 32);
  ASSERT_EQ(Large.Status, FiniteValueStatus::Complete);
  ASSERT_EQ(Large.Tuples.size(), 32u);
  Cache.store(Ctx, P, {X}, 32, Large);
  for (auto Value : Values)
    EXPECT_FALSE(Cache.lookup(Ctx, P, {Value}, 4));
  const auto Held = Cache.lookup(Ctx, P, {X}, 32);
  expectHit(Held, Large);
  const auto Replacement = prove(Ctx, P, {Values[0]}, 4);
  Cache.store(Ctx, P, {Values[0]}, 4, Replacement);
  EXPECT_FALSE(Cache.lookup(Ctx, P, {X}, 32));
  expectHit(Cache.lookup(Ctx, P, {Values[0]}, 4), Replacement);
  expectHit(Held, Large);
}

TEST(FiniteQueryCache, OversizedDagAndInvalidQueriesCannotHit) {
  SymContext Ctx;
  const auto X = Ctx.mkVar("x", 8);
  auto Deep = X;
  for (unsigned I = 1; I < 32; ++I)
    Deep = Ctx.mkUDiv(X, Ctx.mkAdd(Deep, Ctx.mkConst(8, I)));
  const auto P = Ctx.mkFalse();
  const auto Result = prove(Ctx, P, {Deep}, 2);
  FiniteQueryCache Cache(64);
  Cache.store(Ctx, P, {Deep}, 2, Result);
  EXPECT_FALSE(Cache.lookup(Ctx, P, {Deep}, 2));
  EXPECT_FALSE(Cache.lookup(Ctx, {}, {X}, 2));
  EXPECT_FALSE(Cache.lookup(Ctx, X, {X}, 2));
  EXPECT_FALSE(Cache.lookup(Ctx, P, {SymRef(0xfffffffe)}, 2));
  EXPECT_FALSE(Cache.lookup(Ctx, P, {X}, 0));
}

TEST(FiniteQueryCache, ByteOrderIsRepresentedByTheExpressionDag) {
  FiniteQueryCache Cache(4096);
  SymContext Little;
  SymState LittleState(Little, llvm::endianness::little);
  const auto Word = LittleState.read(SymSpace::Register, 24, 2);
  const auto Byte = LittleState.read(SymSpace::Register, 24, 1);
  const auto P = Little.mkEq(Word, Little.mkConst(16, 0x0102));
  const auto Result = prove(Little, P, {Byte}, 4);
  ASSERT_EQ(Result.Tuples, (std::vector<std::vector<uint64_t>>{{2}}));
  Cache.store(Little, P, {Byte}, 4, Result);

  SymContext Big;
  SymState BigState(Big, llvm::endianness::big);
  const auto OtherWord = BigState.read(SymSpace::Register, 24, 2);
  const auto OtherByte = BigState.read(SymSpace::Register, 24, 1);
  const auto Q = Big.mkEq(OtherWord, Big.mkConst(16, 0x0102));
  EXPECT_FALSE(Cache.lookup(Big, Q, {OtherByte}, 4));
  EXPECT_EQ(prove(Big, Q, {OtherByte}, 4).Tuples,
            (std::vector<std::vector<uint64_t>>{{1}}));
}
} // namespace

namespace {
using Cache = FiniteQueryCache;
TEST(PreparedFiniteKeys, OwnsCompleteKeyAndWidthsAfterContextDestruction) {
  Cache C(4096);
  Cache::PreparedQuery Q;
  FiniteValues Result;
  {
    SymContext S;
    auto X = S.mkVar("old_x", 8), Y = S.mkVar("old_y", 8);
    auto P = S.mkAnd(S.mkEq(S.mkAdd(X, Y), S.mkConst(8, 9)),
                     S.mkUlt(X, S.mkConst(8, 2)));
    Q = C.prepare(S, P, {X, Y}, 2);
    EXPECT_FALSE(C.lookup(Q));
    Result = prove(S, P, {X, Y}, 2);
    ASSERT_EQ(Result.Status, FiniteValueStatus::Complete);
    ASSERT_EQ(Result.Tuples,
              (std::vector<std::vector<uint64_t>>{{0, 9}, {1, 8}}));
  }
  C.store(std::move(Q), Result);
  SymContext S;
  auto X = S.mkVar("renamed_x", 8), Y = S.mkVar("renamed_y", 8);
  auto P = S.mkAnd(S.mkEq(S.mkAdd(X, Y), S.mkConst(8, 9)),
                   S.mkUlt(X, S.mkConst(8, 2)));
  auto Again = C.prepare(S, P, {X, Y}, 2);
  auto Hit = C.lookup(Again);
  ASSERT_TRUE(Hit);
  EXPECT_EQ(Hit->Tuples, Result.Tuples);
  EXPECT_FALSE(C.lookup(C.prepare(S, P, {Y, X}, 2)));
  EXPECT_FALSE(C.lookup(C.prepare(S, P, {X, Y}, 1)));
}
TEST(PreparedFiniteKeys, InvalidIncompleteAndMalformedResultsDoNotEnterCache) {
  SymContext S;
  auto X = S.mkVar("x", 8);
  auto P = S.mkEq(X, S.mkConst(8, 5));
  const auto Good = prove(S, P, {X}, 1);
  ASSERT_EQ(Good.Status, FiniteValueStatus::Complete);
  for (auto Status : {FiniteValueStatus::Unknown, FiniteValueStatus::Invalid,
                      FiniteValueStatus::QueryBudgetExceeded}) {
    Cache C(4096);
    C.store(C.prepare(S, P, {X}, 1), {Status, {}});
    EXPECT_FALSE(C.lookup(C.prepare(S, P, {X}, 1)));
  }
  for (auto Tuples :
       {std::vector<std::vector<uint64_t>>{{256}}, {{5, 5}}, {{5}, {5}}}) {
    Cache C(4096);
    C.store(C.prepare(S, P, {X}, 1), {FiniteValueStatus::Complete, Tuples});
    EXPECT_FALSE(C.lookup(C.prepare(S, P, {X}, 1)));
  }
  Cache C(4096);
  C.store(Cache::PreparedQuery{}, Good);
  C.store(C.prepare(S, {}, {X}, 1), Good);
  EXPECT_FALSE(C.lookup(C.prepare(S, P, {X}, 1)));
  auto Source = C.prepare(S, P, {X}, 1);
  auto Destination = std::move(Source);
  C.store(std::move(Source), Good);
  EXPECT_FALSE(C.lookup(C.prepare(S, P, {X}, 1)));
  EXPECT_FALSE(C.lookup(Source));
  Cache::PreparedQuery Assigned;
  Assigned = std::move(Destination);
  EXPECT_FALSE(C.lookup(Destination));
  auto *Self = &Assigned;
  Assigned = std::move(*Self);
  C.store(std::move(Assigned), Good);
  EXPECT_FALSE(C.lookup(Assigned));
  ASSERT_TRUE(C.lookup(C.prepare(S, P, {X}, 1)));
}
TEST(PreparedFiniteKeys, OriginalCapacityAccountingAndTargetLimitsRemainExact) {
  SymContext S;
  auto X = S.mkVar("x", 8);
  auto P = S.mkEq(X, S.mkConst(8, 5));
  const auto Good = prove(S, P, {X}, 1);
  unsigned Fits = 0, Rejects = 0;
  for (uint64_t Budget = 0; Budget != 256; ++Budget) {
    Cache A(Budget), B(Budget);
    A.store(S, P, {X}, 1, Good);
    B.store(B.prepare(S, P, {X}, 1), Good);
    auto HA = A.lookup(S, P, {X}, 1), HB = B.lookup(B.prepare(S, P, {X}, 1));
    EXPECT_EQ(bool(HA), bool(HB));
    if (HA) {
      ++Fits;
      EXPECT_EQ(HA->Tuples, HB->Tuples);
    } else
      ++Rejects;
  }
  EXPECT_GT(Fits, 0U);
  EXPECT_GT(Rejects, 0U);
  Cache Large(4096), Small(1);
  auto Q = Large.prepare(S, P, {X}, 1);
  EXPECT_FALSE(Small.lookup(Q));
  Small.store(std::move(Q), Good);
  EXPECT_FALSE(Small.lookup(Large.prepare(S, P, {X}, 1)));
}
TEST(PreparedFiniteKeys, EmptyAndNonuniqueProofsKeepTheirExactDomains) {
  SymContext S;
  auto X = S.mkVar("x", 8);
  Cache C(4096);
  const auto Empty = prove(S, S.mkFalse(), {X}, 1);
  C.store(C.prepare(S, S.mkFalse(), {X}, 1), Empty);
  auto H = C.lookup(C.prepare(S, S.mkFalse(), {X}, 1));
  ASSERT_TRUE(H);
  EXPECT_EQ(H->Status, FiniteValueStatus::Complete);
  EXPECT_TRUE(H->Tuples.empty());
  const auto Many = prove(S, S.mkTrue(), {X}, 1);
  ASSERT_EQ(Many.Status, FiniteValueStatus::TooManyValues);
  C.store(C.prepare(S, S.mkTrue(), {X}, 1), Many);
  H = C.lookup(C.prepare(S, S.mkTrue(), {X}, 1));
  ASSERT_TRUE(H);
  EXPECT_EQ(H->Status, FiniteValueStatus::TooManyValues);
  EXPECT_FALSE(C.lookup(C.prepare(S, S.mkTrue(), {}, 1)));
}
} // namespace

namespace {
TEST(ContextFiniteProofs, ContextOwnerAndModeAreSeparate) {
  SymContext A, B;
  const auto X = A.mkVar("x", 8), Y = B.mkVar("x", 8);
  const auto P = A.mkEq(X, A.mkConst(8, 3));
  const auto Q = B.mkEq(Y, B.mkConst(8, 7));
  ASSERT_EQ(X.index(), Y.index());
  ASSERT_EQ(P.index(), Q.index());
  const auto R = prove(A, P, {X}, 1), Other = prove(B, Q, {Y}, 1);
  ASSERT_EQ(R.Tuples, (std::vector<std::vector<uint64_t>>{{3}}));
  ASSERT_EQ(Other.Tuples, (std::vector<std::vector<uint64_t>>{{7}}));
  Cache C(4096, A), Sibling(4096, A), Foreign(4096, B), Structural(4096);
  C.store(C.prepare(A, P, {X}, 1), R);
  expectHit(C.lookup(A, P, {X}, 1), R);
  EXPECT_FALSE(C.lookup(B, Q, {Y}, 1));
  C.store(C.prepare(B, Q, {Y}, 1), Other);
  expectHit(C.lookup(A, P, {X}, 1), R);
  for (auto *Target : {&Sibling, &Foreign, &Structural}) {
    auto Token = C.prepare(A, P, {X}, 1);
    EXPECT_FALSE(Target->lookup(Token));
    Target->store(std::move(Token), R);
    EXPECT_FALSE(Target->lookup(A, P, {X}, 1));
    EXPECT_FALSE(Target->lookup(B, Q, {Y}, 1));
  }
  Structural.store(Structural.prepare(A, P, {X}, 1), R);
  EXPECT_FALSE(C.lookup(Structural.prepare(A, P, {X}, 1)));
  Sibling.store(Structural.prepare(A, P, {X}, 1), R);
  EXPECT_FALSE(Sibling.lookup(A, P, {X}, 1));
  Foreign.store(Foreign.prepare(B, Q, {Y}, 1), Other);
  expectHit(Foreign.lookup(B, Q, {Y}, 1), Other);
}

TEST(ContextFiniteProofs, ReplacedOwnerCannotConsumeOldTokens) {
  SymContext Ctx;
  const auto X = Ctx.mkVar("x", 8), P = Ctx.mkEq(X, Ctx.mkConst(8, 5));
  const auto R = prove(Ctx, P, {X}, 1);
  std::optional<Cache> Owner(std::in_place, 4096, Ctx);
  const auto *Address = &*Owner;
  auto Old = Owner->prepare(Ctx, P, {X}, 1);
  Owner->store(Owner->prepare(Ctx, P, {X}, 1), R);
  Owner.reset();
  Owner.emplace(4096, Ctx);
  ASSERT_EQ(Address, &*Owner);
  Owner->store(Owner->prepare(Ctx, P, {X}, 1), R);
  EXPECT_FALSE(Owner->lookup(Old));
  Owner->store(std::move(Old), R);
  expectHit(Owner->lookup(Ctx, P, {X}, 1), R);
}

TEST(ContextFiniteProofs, MovesInvalidateSourcesAndPreserveOwner) {
  SymContext Ctx;
  const auto X = Ctx.mkVar("x", 8), P = Ctx.mkEq(X, Ctx.mkConst(8, 5));
  const auto R = prove(Ctx, P, {X}, 1);
  Cache C(4096, Ctx), Other(4096, Ctx);
  auto Q = C.prepare(Ctx, P, {X}, 1);
  auto Moved = std::move(Q);
  Cache::PreparedQuery Assigned;
  Assigned = std::move(Moved);
  auto *Self = &Assigned;
  Assigned = std::move(*Self);
  EXPECT_FALSE(Other.lookup(Assigned));
  C.store(std::move(Assigned), R);
  EXPECT_FALSE(C.lookup(Q));
  EXPECT_FALSE(C.lookup(Moved));
  EXPECT_FALSE(C.lookup(Assigned));
  expectHit(C.lookup(Ctx, P, {X}, 1), R);
}

TEST(ContextFiniteProofs, ExactPredicateProjectionOrderArityAndLimit) {
  SymContext Ctx;
  const auto X = Ctx.mkVar("x", 8), Y = Ctx.mkVar("y", 8);
  const auto PX = Ctx.mkEq(X, Ctx.mkConst(8, 3));
  const auto P = Ctx.mkAnd(PX, Ctx.mkEq(Y, Ctx.mkConst(8, 7)));
  Cache C(4096, Ctx);
  const auto R = prove(Ctx, P, {X, Y}, 1);
  ASSERT_EQ(R.Tuples, (std::vector<std::vector<uint64_t>>{{3, 7}}));
  C.store(C.prepare(Ctx, P, {X, Y}, 1), R);
  EXPECT_FALSE(C.lookup(Ctx, P, {Y, X}, 1));
  EXPECT_FALSE(C.lookup(Ctx, P, {X, X}, 1));
  EXPECT_FALSE(C.lookup(Ctx, P, {X}, 1));
  EXPECT_FALSE(C.lookup(Ctx, P, {}, 1));
  EXPECT_FALSE(C.lookup(Ctx, P, {X, Y}, 2));
  EXPECT_FALSE(C.lookup(Ctx, PX, {X, Y}, 1));
  EXPECT_FALSE(C.lookup(Ctx, Ctx.mkFalse(), {X, Y}, 1));
  for (unsigned I = 0; I != 2048; ++I)
    Ctx.mkFreshVar(64);
  expectHit(C.lookup(Ctx, P, {X, Y}, 1), R);
  EXPECT_EQ(prove(Ctx, P, {X, Y}, 1).Tuples, R.Tuples);
}

TEST(ContextFiniteProofs, CompleteEmptyAndNonuniqueResultsRemainDistinct) {
  SymContext Ctx;
  const auto X = Ctx.mkVar("x", 8);
  Cache C(4096, Ctx);
  const auto Empty = prove(Ctx, Ctx.mkFalse(), {X}, 1);
  const auto Many = prove(Ctx, Ctx.mkTrue(), {X}, 1);
  const auto Unit = prove(Ctx, Ctx.mkTrue(), {}, 1);
  ASSERT_EQ(Empty.Status, FiniteValueStatus::Complete);
  ASSERT_TRUE(Empty.Tuples.empty());
  ASSERT_EQ(Many.Status, FiniteValueStatus::TooManyValues);
  ASSERT_TRUE(Many.Tuples.empty());
  ASSERT_EQ(Unit.Tuples, (std::vector<std::vector<uint64_t>>{{}}));
  C.store(C.prepare(Ctx, Ctx.mkFalse(), {X}, 1), Empty);
  C.store(C.prepare(Ctx, Ctx.mkTrue(), {X}, 1), Many);
  C.store(C.prepare(Ctx, Ctx.mkTrue(), {}, 1), Unit);
  expectHit(C.lookup(Ctx, Ctx.mkFalse(), {X}, 1), Empty);
  expectHit(C.lookup(Ctx, Ctx.mkTrue(), {X}, 1), Many);
  expectHit(C.lookup(Ctx, Ctx.mkTrue(), {}, 1), Unit);
}

TEST(ContextFiniteProofs, ResultValidationAndExactStorageCeilings) {
  SymContext Ctx;
  const auto X = Ctx.mkVar("x", 1), P = Ctx.mkTrue();
  for (const FiniteValues R :
       {FiniteValues{FiniteValueStatus::Unknown, {{0}}},
        FiniteValues{FiniteValueStatus::Invalid, {}},
        FiniteValues{FiniteValueStatus::QueryBudgetExceeded, {{0}}},
        FiniteValues{FiniteValueStatus::Complete, {{2}}},
        FiniteValues{FiniteValueStatus::Complete, {{0, 1}}},
        FiniteValues{FiniteValueStatus::Complete, {{0}, {1}}},
        FiniteValues{FiniteValueStatus::TooManyValues, {{0}}}}) {
    Cache C(4096, Ctx);
    C.store(C.prepare(Ctx, P, {X}, 1), R);
    EXPECT_FALSE(C.lookup(Ctx, P, {X}, 1));
  }
  const auto V = Ctx.mkConst(8, 7);
  const auto R = prove(Ctx, P, {V}, 1);
  for (unsigned Budget = 0; Budget != 23; ++Budget) {
    Cache C(Budget, Ctx);
    C.store(C.prepare(Ctx, P, {V}, 1), R);
    const auto H = C.lookup(Ctx, P, {V}, 1);
    EXPECT_EQ(bool(H), Budget >= 21);
    if (H)
      expectHit(H, R);
  }
}

TEST(ContextFiniteProofs, InvalidKeysCannotEvictOrRefreshStoredProofs) {
  SymContext Ctx;
  Cache C(42, Ctx);
  const auto P = Ctx.mkTrue(), A = Ctx.mkConst(8, 7), B = Ctx.mkConst(8, 8);
  const auto D = Ctx.mkConst(8, 9);
  const auto RA = prove(Ctx, P, {A}, 1), RB = prove(Ctx, P, {B}, 1);
  const auto RD = prove(Ctx, P, {D}, 1);
  C.store(C.prepare(Ctx, P, {A}, 1), RA);
  C.store(C.prepare(Ctx, P, {B}, 1), RB);
  EXPECT_FALSE(C.lookup(Ctx, {}, {A}, 1));
  EXPECT_FALSE(C.lookup(Ctx, A, {A}, 1));
  EXPECT_FALSE(C.lookup(Ctx, P, {A}, 0));
  EXPECT_FALSE(C.lookup(Ctx, P, {SymRef(0xfffffffe)}, 1));
  EXPECT_FALSE(C.lookup(Ctx, P, {Ctx.mkVar("wide", 128)}, 1));
  C.store(C.prepare(Ctx, P, std::vector<SymRef>(40, A), 1), RA);
  C.store(C.prepare(Ctx, P, {D}, 1), RD);
  EXPECT_FALSE(C.lookup(Ctx, P, {A}, 1));
  expectHit(C.lookup(Ctx, P, {B}, 1), RB);
  const auto Retained = C.lookup(Ctx, P, {D}, 1);
  expectHit(Retained, RD);
  expectHit(C.lookup(Ctx, P, {B}, 1), RB);
  C.store(C.prepare(Ctx, P, {A}, 1), RA);
  EXPECT_FALSE(C.lookup(Ctx, P, {D}, 1));
  expectHit(Retained, RD);
}

TEST(ContextFiniteProofs, PartialEnumerationNeverSuppliesAFrameProof) {
  SymContext Ctx;
  const auto Root = Ctx.mkVar("root", 64), V = Ctx.mkVar("v", 64);
  const auto P = Ctx.mkEq(V, Ctx.mkAdd(Root, Ctx.mkConst(64, 16)));
  Cache C(4096, Ctx);
  FiniteDomainEncoding Encoding(Ctx, {});
  uint64_t Queries = 0;
  const auto Short =
      proveFrameOffset(Encoding, P, V, Root, 1, 65536, Queries, &C);
  EXPECT_EQ(Short.Status, FrameOffsetStatus::BudgetExceeded);
  EXPECT_EQ(Queries, 1u);
  Queries = 0;
  const auto Complete =
      proveFrameOffset(Encoding, P, V, Root, 2, 65536, Queries, &C);
  ASSERT_EQ(Complete.Status, FrameOffsetStatus::Exact);
  EXPECT_EQ(Complete.Offset, 16u);
  EXPECT_EQ(Queries, 2u);
  Queries = 0;
  const auto Cached =
      proveFrameOffset(Encoding, P, V, Root, 0, 65536, Queries, &C);
  EXPECT_EQ(Cached.Status, Complete.Status);
  EXPECT_EQ(Cached.Offset, Complete.Offset);
  EXPECT_EQ(Queries, 0u);
  const auto TooSmall = proveFrameOffset(Encoding, P, V, Root, 0,
                                         Ctx.numNodes() - 1, Queries, &C);
  EXPECT_EQ(TooSmall.Status, FrameOffsetStatus::BudgetExceeded);
  const auto Unconstrained =
      proveFrameOffset(Encoding, Ctx.mkTrue(), V, Root, 0, 65536, Queries, &C);
  EXPECT_EQ(Unconstrained.Status, FrameOffsetStatus::BudgetExceeded);
}

TEST(ContextFiniteProofs, UnknownAndInvalidAuthoritativeProofsAreNotCached) {
  for (unsigned Kind = 0; Kind != 3; ++Kind) {
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", 8), Y = Ctx.mkVar("y", 8);
    const auto P = Ctx.mkEq(Ctx.mkAdd(X, Y), Ctx.mkConst(8, 9));
    Cache C(4096, Ctx);
    SpecializationOptions Options;
    if (Kind == 0)
      Options.MaxSolverGates = 1;
    if (Kind == 1)
      Options.MaxSolverQueries = 1;
    const SymRef Value = Kind == 2 ? SymRef(0xfffffffe) : X;
    uint64_t Queries = 0;
    auto Token = C.prepare(Ctx, P, {Value}, 1);
    const auto R = enumerateFiniteValues(Ctx, P, {Value}, 1, Options, Queries);
    EXPECT_NE(R.Status, FiniteValueStatus::Complete);
    EXPECT_NE(R.Status, FiniteValueStatus::TooManyValues);
    EXPECT_TRUE(R.Tuples.empty());
    C.store(std::move(Token), R);
    EXPECT_FALSE(C.lookup(Ctx, P, {Value}, 1));
  }
}
} // namespace
