//===- SymLoadOriginBindingTests.cpp - Symbolic state contracts
//---------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymState.h"

#include <algorithm>
#include <limits>

using namespace neverd::symbolic;

TEST(SymLoadOriginBinding, AddressWidthsFollowTheExistingLoadContract) {
  for (unsigned Width : {32u, 64u, 128u}) {
    SymContext C;
    SymState State(C);
    auto Address = C.mkVar("address", Width);
    auto Original = State.load(Address, 4);
    auto Rebound = C.mkVar("rebound", 32);
    auto Origin = *State.loadOrigin(Original);
    State.clobberMemory();
    ASSERT_TRUE(State.bindLoadOrigins({{Rebound, Origin}}));
    ASSERT_NE(State.loadOrigin(Rebound), nullptr);
    EXPECT_EQ(*State.loadOrigin(Rebound), Origin);
    EXPECT_EQ(State.numLiveBytes(), 0u);
    auto Bad = SymRef(uint32_t(C.numNodes() + 20));
    EXPECT_FALSE(State.bindLoadOrigins({{Rebound, {Bad, 4}}}));
    EXPECT_EQ(*State.loadOrigin(Rebound), Origin);
  }
}

TEST(SymLoadOriginBinding, ReboundHistoryNeverSeedsOrRevivesAvailableBytes) {
  SymContext ctx;
  SymState state(ctx);
  auto address = ctx.mkConst(64, 0x1000);
  auto old = state.load(address, 8);
  auto parameter = ctx.mkFreshVar(64, "parameter_");
  auto origin = *state.loadOrigin(old);
  state.clobberMemory();
  auto sibling = state;
  ASSERT_TRUE(state.bindLoadOrigins({{parameter, origin}}));
  ASSERT_NE(state.loadOrigin(parameter), nullptr);
  EXPECT_EQ(*state.loadOrigin(parameter), origin);
  EXPECT_EQ(state.loadOrigin(old)->Address, address);
  EXPECT_EQ(sibling.loadOrigin(parameter), nullptr);
  EXPECT_EQ(state.numLiveBytes(), 0u);
  auto current = state.load(address, 8);
  EXPECT_NE(current, old);
  EXPECT_NE(current, parameter);
}

TEST(SymLoadOriginBinding, ConflictsAreAnOrderIndependentUnion) {
  SymContext ctx;
  auto value = ctx.mkFreshVar(64, "rebound_");
  SymState::LoadOrigin a{ctx.mkConst(64, 0x1000), 8};
  SymState::LoadOrigin b{ctx.mkConst(64, 0x2000), 8};
  SymState forward(ctx), reverse(ctx);
  ASSERT_TRUE(forward.bindLoadOrigins({{value, a}, {value, b}, {value, a}}));
  ASSERT_TRUE(reverse.bindLoadOrigins({{value, b}, {value, a}, {value, b}}));
  EXPECT_EQ(forward.loadOrigins(value), reverse.loadOrigins(value));
  EXPECT_EQ(forward.loadOrigins(value).size(), 2u);
  EXPECT_EQ(forward.loadOrigin(value), nullptr);
  EXPECT_EQ(forward.numLiveBytes(), 0u);
}

TEST(SymLoadOriginBinding, InvalidBatchCannotPublishAPartialHistory) {
  SymContext ctx;
  auto value = ctx.mkFreshVar(64, "rebound_");
  auto a = ctx.mkConst(64, 0x1000), b = ctx.mkConst(64, 0x2000);
  for (auto invalid : std::vector<SymState::LoadOriginBinding>{
           {{}, {b, 8}},
           {value, {{}, 8}},
           {value, {b, 0}},
           {value, {b, 4}},
           {ctx.mkFreshVar(32, "narrow_"), {b, 8}}}) {
    SymState state(ctx);
    EXPECT_FALSE(state.bindLoadOrigins({{value, {a, 8}}, invalid}));
    EXPECT_TRUE(state.loadOrigins(value).empty());
    EXPECT_EQ(state.numLiveBytes(), 0u);
  }
}
