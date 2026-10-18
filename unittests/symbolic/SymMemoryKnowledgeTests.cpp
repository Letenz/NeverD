//===- SymMemoryKnowledgeTests.cpp - Symbolic state contracts
//---------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymState.h"

#include <algorithm>
#include <limits>

using namespace neverd::symbolic;

TEST(SymMemoryKnowledge, RelativeCoordinatesFollowModularBankWrites) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext C;
    SymState State(C, Order), Copy(C, Order);
    auto P = C.mkVar("p", 64);
    auto Last = std::numeric_limits<uint64_t>::max();
    SymState::MemoryKnowledge Facts{
        &C, Order, {{P, Last, C.mkConst(16, 0x2211)}}};
    ASSERT_TRUE(State.restoreMemoryKnowledge(Facts));
    EXPECT_EQ(State.load(C.mkAdd(P, C.mkConst(64, Last)), 2),
              C.mkConst(16, 0x2211));
    EXPECT_EQ(State.load(P, 1),
              C.mkConst(8, Order == llvm::endianness::little ? 0x22 : 0x11));
    ASSERT_TRUE(Copy.restoreMemoryKnowledge(State.memoryKnowledge()));
    EXPECT_EQ(Copy.load(C.mkAdd(P, C.mkConst(64, Last)), 2),
              C.mkConst(16, 0x2211));
    Facts.Facts = {{C.mkAdd(P, C.mkConst(64, Last)), 2, C.mkConst(8, 0x77)}};
    ASSERT_TRUE(State.restoreMemoryKnowledge(Facts));
    EXPECT_EQ(State.load(C.mkAdd(P, C.mkOne(64)), 1), C.mkConst(8, 0x77));
  }
}

TEST(SymMemoryKnowledge, AbsoluteBaseAdditionCannotWrapBeforeValidation) {
  SymContext C;
  SymState State(C);
  auto Address = C.mkConst(64, 0x1000);
  ASSERT_TRUE(State.store(Address, C.mkConst(32, 0x44332211)));
  auto Facts = State.memoryKnowledge();
  Facts.Facts.push_back({C.mkConst(64, std::numeric_limits<uint64_t>::max()), 1,
                         C.mkConst(8, 0xaa)});
  EXPECT_FALSE(State.restoreMemoryKnowledge(Facts));
  EXPECT_EQ(State.load(Address, 4), C.mkConst(32, 0x44332211));
  EXPECT_FALSE(State.memoryIsUnknown());
}

TEST(SymMemoryKnowledge,
     InvalidReferencesAndNonByteValuesRejectTheWholeImport) {
  SymContext C;
  SymState State(C);
  auto P = C.mkVar("p", 64);
  State.store(P, C.mkConst(16, 0x1234));
  auto Good = State.memoryKnowledge();
  auto BadRef = SymRef(uint32_t(C.numNodes() + 20));
  for (auto Fact :
       std::vector<SymState::MemoryFact>{{BadRef, 0, C.mkConst(8, 1)},
                                         {P, 0, BadRef},
                                         {P, 0, C.mkConst(1, 1)},
                                         {P, 0, C.mkConst(17, 1)}}) {
    auto Bad = Good;
    Bad.Facts.push_back(Fact);
    EXPECT_FALSE(State.restoreMemoryKnowledge(Bad));
    EXPECT_EQ(State.load(P, 2), C.mkConst(16, 0x1234));
  }
}

TEST(SymMemoryKnowledge, ByteOracleAndScalarBanksSurviveRoundTrips) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    for (unsigned Width : {32u, 64u, 128u}) {
      SymContext C;
      SymState Source(C, Order), Target(C, Order);
      auto P = C.mkVar("p", Width);
      for (unsigned I = 0; I != 19; ++I)
        Source.write(SymSpace::Memory, 0x1000 + I, C.mkConst(8, I * 7 + 3));
      auto Region = Source.load(P, 3);
      Target.write(SymSpace::Register, 8, C.mkConst(64, 0x77));
      Target.write(SymSpace::Temporary, 24, C.mkConst(32, 0x99));
      ASSERT_TRUE(Target.restoreMemoryKnowledge(Source.memoryKnowledge()));
      EXPECT_EQ(Target.load(P, 3), Region);
      EXPECT_EQ(Target.read(SymSpace::Register, 8, 8), C.mkConst(64, 0x77));
      EXPECT_EQ(Target.read(SymSpace::Temporary, 24, 4), C.mkConst(32, 0x99));
      for (unsigned Start = 0; Start != 12; ++Start) {
        uint64_t Want = 0;
        for (unsigned I = 0; I != 8; ++I)
          Want |= uint64_t((Start + I) * 7 + 3)
                  << (8 * (Order == llvm::endianness::little ? I : 7 - I));
        EXPECT_EQ(Target.load(C.mkConst(64, 0x1000 + Start), 8),
                  C.mkConst(64, Want));
      }
    }
  }
}

TEST(SymMemoryKnowledge, RoundTripRetainsSeparateRegionsAndByteOrder) {
  for (auto order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext ctx;
    SymState source(ctx, order), target(ctx, order);
    auto p = ctx.mkVar("p", 64), q = ctx.mkVar("q", 64);
    auto absolute = ctx.mkConst(64, 0x1000);
    auto a = source.load(absolute, 8), b = source.load(p, 8),
         c = source.load(q, 8);
    auto live = source.numLiveBytes();
    auto knowledge = source.memoryKnowledge();
    EXPECT_EQ(source.numLiveBytes(), live);
    ASSERT_EQ(knowledge.Facts.size(), 3u);
    ASSERT_TRUE(target.restoreMemoryKnowledge(knowledge));
    EXPECT_EQ(target.load(absolute, 8), a);
    EXPECT_EQ(target.load(p, 8), b);
    EXPECT_EQ(target.load(q, 8), c);
    EXPECT_EQ(
        target.load(ctx.mkAdd(p, ctx.mkConst(64, 2)), 2),
        ctx.mkExtract(b, order == llvm::endianness::little ? 16 : 32, 16));
  }
}

TEST(SymMemoryKnowledge,
     RebindingBasesPreservesDisplacementsAndCoexistingFacts) {
  SymContext ctx;
  SymState source(ctx), target(ctx);
  auto p = ctx.mkVar("p", 64), q = ctx.mkVar("q", 64);
  auto p2 = ctx.mkVar("p2", 64), q2 = ctx.mkVar("q2", 64);
  source.load(p, 16);
  source.load(q, 8);
  auto knowledge = source.memoryKnowledge();
  ASSERT_EQ(knowledge.Facts.size(), 3u);
  for (auto &fact : knowledge.Facts) {
    fact.Base = fact.Base == p ? p2 : q2;
    fact.Value = ctx.mkConst(64, fact.Base == p2 ? 100 + fact.Offset : 200);
  }
  ASSERT_TRUE(target.restoreMemoryKnowledge(knowledge));
  EXPECT_EQ(target.load(p2, 8), ctx.mkConst(64, 100));
  EXPECT_EQ(target.load(ctx.mkAdd(p2, ctx.mkConst(64, 8)), 8),
            ctx.mkConst(64, 108));
  EXPECT_EQ(target.load(q2, 8), ctx.mkConst(64, 200));
  EXPECT_NE(target.load(p, 8), ctx.mkConst(64, 100));
}

TEST(SymMemoryKnowledge, ExactCollisionsAreOrderIndependentAndTransactional) {
  SymContext ctx;
  auto p = ctx.mkVar("p", 64);
  auto a = ctx.mkConst(64, 0x1000);
  SymState::MemoryKnowledge knowledge{&ctx,
                                      llvm::endianness::little,
                                      {{a, 0, ctx.mkConst(32, 0x44332211)},
                                       {{}, 0x1001, ctx.mkConst(16, 0x3322)}}};
  for (bool reverse : {false, true}) {
    if (reverse)
      std::reverse(knowledge.Facts.begin(), knowledge.Facts.end());
    SymState state(ctx);
    auto old = state.load(p, 8);
    ASSERT_TRUE(state.restoreMemoryKnowledge(knowledge));
    EXPECT_EQ(state.load(a, 4), ctx.mkConst(32, 0x44332211));
    auto conflict = knowledge;
    for (auto &fact : conflict.Facts)
      if (ctx.width(fact.Value) == 16)
        fact.Value = ctx.mkConst(16, 0x99aa);
    auto before = state.load(p, 8);
    EXPECT_FALSE(state.restoreMemoryKnowledge(conflict));
    EXPECT_EQ(state.load(a, 4), ctx.mkConst(32, 0x44332211));
    EXPECT_EQ(state.load(p, 8), before);
    EXPECT_NE(before, old);
  }
}

TEST(SymMemoryKnowledge, InvalidSnapshotsCannotPartlyUpdateTheState) {
  SymContext ctx, other;
  SymState state(ctx);
  auto p = ctx.mkVar("p", 64);
  state.store(p, ctx.mkConst(64, 17));
  auto good = state.memoryKnowledge();
  auto bad = good;
  bad.Owner = &other;
  EXPECT_FALSE(state.restoreMemoryKnowledge(bad));
  bad = good;
  bad.Order = llvm::endianness::big;
  EXPECT_FALSE(state.restoreMemoryKnowledge(bad));
  bad = good;
  bad.Facts.push_back({{}, 0, {}});
  EXPECT_FALSE(state.restoreMemoryKnowledge(bad));
  bad = good;
  bad.Facts.push_back(
      {{}, std::numeric_limits<uint64_t>::max(), ctx.mkConst(16, 0)});
  EXPECT_FALSE(state.restoreMemoryKnowledge(bad));
  EXPECT_EQ(state.load(p, 8), ctx.mkConst(64, 17));
}

TEST(SymMemoryKnowledge, ReboundSymbolicRegionsMergeOnlyCompatibleBytes) {
  SymContext ctx;
  auto p = ctx.mkVar("p", 64);
  SymState::MemoryKnowledge knowledge{
      &ctx,
      llvm::endianness::little,
      {{ctx.mkAdd(p, ctx.mkConst(64, 8)), 0, ctx.mkConst(32, 0x44332211)},
       {p, 9, ctx.mkConst(16, 0x3322)}}};
  for (bool reverse : {false, true}) {
    if (reverse)
      std::reverse(knowledge.Facts.begin(), knowledge.Facts.end());
    SymState state(ctx);
    ASSERT_TRUE(state.restoreMemoryKnowledge(knowledge));
    EXPECT_EQ(state.load(ctx.mkAdd(p, ctx.mkConst(64, 8)), 4),
              ctx.mkConst(32, 0x44332211));
    auto bad = knowledge;
    for (auto &fact : bad.Facts)
      if (ctx.width(fact.Value) == 16)
        fact.Value = ctx.mkConst(16, 0x1234);
    EXPECT_FALSE(state.restoreMemoryKnowledge(bad));
    EXPECT_EQ(state.load(ctx.mkAdd(p, ctx.mkConst(64, 8)), 4),
              ctx.mkConst(32, 0x44332211));
  }
}

TEST(SymMemoryKnowledge,
     HistoricalOriginsAndSharedLazyBytesAreNotCurrentFacts) {
  SymContext ctx;
  SymState source(ctx);
  auto p = ctx.mkVar("p", 64);
  auto old = source.load(p, 8);
  source.write(SymSpace::Register, 0, old);
  source.clobberMemory();
  ASSERT_NE(source.loadOrigin(old), nullptr);
  EXPECT_TRUE(source.memoryKnowledge().Facts.empty());
  SymState sibling(source);
  sibling.load(ctx.mkVar("q", 64), 8);
  EXPECT_TRUE(source.memoryKnowledge().Facts.empty());
  auto empty = source.memoryKnowledge();
  ASSERT_TRUE(source.restoreMemoryKnowledge(empty));
  EXPECT_EQ(source.read(SymSpace::Register, 0, 8), old);
  EXPECT_NE(source.loadOrigin(old), nullptr);
  EXPECT_NE(source.load(p, 8), old);
}

TEST(SymMemoryKnowledge,
     ImportedUnknownEpochsForkButDoNotCrossIndependentImports) {
  SymContext ctx;
  auto p = ctx.mkVar("p", 64);
  SymState first(ctx), second(ctx);
  auto empty = first.memoryKnowledge();
  ASSERT_TRUE(first.restoreMemoryKnowledge(empty));
  ASSERT_TRUE(second.restoreMemoryKnowledge(empty));
  SymState fork(first);
  auto a = first.load(p, 8);
  EXPECT_EQ(fork.load(p, 8), a);
  EXPECT_NE(second.load(p, 8), a);
}

TEST(SymMemoryKnowledge, LaterWritesAndHavocStillInvalidateMayAliases) {
  SymContext ctx;
  auto p = ctx.mkVar("p", 64), q = ctx.mkVar("q", 64);
  auto a = ctx.mkConst(64, 0x1000);
  SymState source(ctx), target(ctx);
  auto pv = source.load(p, 8), qv = source.load(q, 8), av = source.load(a, 8);
  ASSERT_TRUE(target.restoreMemoryKnowledge(source.memoryKnowledge()));
  target.store(ctx.mkAdd(p, ctx.mkConst(64, 1)), ctx.mkConst(8, 0x55));
  EXPECT_EQ(target.load(p, 1), ctx.mkExtract(pv, 0, 8));
  EXPECT_EQ(target.load(ctx.mkAdd(p, ctx.mkConst(64, 1)), 1),
            ctx.mkConst(8, 0x55));
  EXPECT_NE(target.load(q, 8), qv);
  EXPECT_NE(target.load(a, 8), av);
  auto current = target.load(p, 8);
  target.clobberMemory();
  EXPECT_TRUE(target.memoryKnowledge().Facts.empty());
  EXPECT_NE(target.load(p, 8), current);
}
