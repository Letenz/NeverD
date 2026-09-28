//===- ControlDiscoveryTests.cpp - Bounded control input discovery -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/analysis/interpreter/ControlDiscovery.h"
#include "gtest/gtest.h"

using namespace neverd::analysis;
using namespace neverd::analysis::detail;
using namespace neverd::symbolic;

TEST(ControlDiscovery, ExactRegisterByteSlicesUseMachineByteOrder) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext Ctx;
    SymState State(Ctx, Order);
    auto Word = State.read(SymSpace::Register, 16, 8);
    auto Byte = Ctx.mkExtract(Word, 24, 8);
    auto Nodes = Ctx.numNodes();
    auto R = gatherControlDependencies(State, Byte, {}, 100);
    ASSERT_EQ(R.Status, ControlDiscoveryStatus::Complete);
    ASSERT_EQ(R.RegisterRanges.size(), 1u);
    EXPECT_EQ(R.RegisterRanges[0].Offset,
              Order == llvm::endianness::little ? 19u : 20u);
    EXPECT_EQ(R.RegisterRanges[0].Bytes, 1u);
    EXPECT_EQ(Ctx.numNodes(), Nodes);
  }
}

TEST(ControlDiscovery, PartialBitsNominateOnlyTheirContainingBytes) {
  SymContext Ctx;
  SymState State(Ctx);
  auto R = gatherControlDependencies(
      State, Ctx.mkExtract(State.read(SymSpace::Register, 16, 8), 5, 6), {},
      100);
  ASSERT_EQ(R.Status, ControlDiscoveryStatus::Complete);
  ASSERT_EQ(R.RegisterRanges.size(), 1u);
  EXPECT_EQ(R.RegisterRanges[0].Offset, 16u);
  EXPECT_EQ(R.RegisterRanges[0].Bytes, 2u);
}

TEST(ControlDiscovery, ConcatRetainsTheSelectedLanesOnly) {
  SymContext Ctx;
  SymState State(Ctx);
  auto A = State.read(SymSpace::Register, 16, 8);
  auto B = State.read(SymSpace::Register, 32, 8);
  auto Join = Ctx.mkConcat({Ctx.mkExtract(A, 8, 8), Ctx.mkExtract(B, 40, 8)});
  auto R = gatherControlDependencies(State, Join, {}, 100);
  ASSERT_EQ(R.Status, ControlDiscoveryStatus::Complete);
  ASSERT_EQ(R.RegisterRanges.size(), 2u);
  EXPECT_EQ(R.RegisterRanges[0].Offset, 17u);
  EXPECT_EQ(R.RegisterRanges[1].Offset, 37u);
  EXPECT_EQ(R.RegisterRanges[0].Bytes, 1u);
  EXPECT_EQ(R.RegisterRanges[1].Bytes, 1u);
}

TEST(ControlDiscovery, FrameLoadSlicesUseEntryRelativeOffsets) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext Ctx;
    SymState State(Ctx, Order);
    auto Root = Ctx.mkFreshVar(64, "frame");
    auto Loaded =
        State.load(Ctx.mkAdd(Root, Ctx.mkConst(64, uint64_t(-16))), 8);
    auto R = gatherControlDependencies(State, Ctx.mkExtract(Loaded, 56, 8),
                                       Root, 100);
    ASSERT_EQ(R.Status, ControlDiscoveryStatus::Complete);
    ASSERT_EQ(R.FrameSlots.size(), 1u);
    EXPECT_TRUE(R.RegisterRanges.empty());
    EXPECT_EQ(R.FrameSlots[0].Offset,
              Order == llvm::endianness::little ? -9 : -16);
    EXPECT_EQ(R.FrameSlots[0].Bytes, 1u);
  }
}

TEST(ControlDiscovery, ExternalMemoryOnlyNominatesAddressDependencies) {
  SymContext Ctx;
  SymState State(Ctx);
  auto Pointer = State.read(SymSpace::Register, 24, 8);
  auto Loaded = State.load(Ctx.mkAdd(Pointer, Ctx.mkConst(64, 0x800)), 8);
  auto R = gatherControlDependencies(State, Loaded, {}, 100);
  ASSERT_EQ(R.Status, ControlDiscoveryStatus::UnsupportedOrigin);
  ASSERT_EQ(R.RegisterRanges.size(), 1u);
  EXPECT_EQ(R.RegisterRanges[0].Offset, 24u);
  EXPECT_EQ(R.RegisterRanges[0].Bytes, 8u);
  EXPECT_TRUE(R.FrameSlots.empty());
}

TEST(ControlDiscovery, FreshTemporaryAndAbsoluteInputsAreNotLocations) {
  SymContext Ctx;
  SymState State(Ctx);
  for (auto Value :
       {Ctx.mkFreshVar(64, "reg$16"), State.read(SymSpace::Temporary, 16, 8),
        State.load(Ctx.mkConst(64, 0x800), 8),
        Ctx.mkInputVar("later_register", 64,
                       {SymInputKind::Register, 16, 8, 1})}) {
    auto R = gatherControlDependencies(State, Value, {}, 100);
    EXPECT_EQ(R.Status, ControlDiscoveryStatus::UnsupportedOrigin);
    EXPECT_TRUE(R.RegisterRanges.empty());
    EXPECT_TRUE(R.FrameSlots.empty());
  }
}

TEST(ControlDiscovery, BudgetExhaustionClearsEveryCandidate) {
  SymContext Ctx;
  SymState State(Ctx);
  auto A = State.read(SymSpace::Register, 16, 8);
  auto B = State.read(SymSpace::Register, 32, 8);
  auto Value = Ctx.mkAdd(A, B);
  for (uint64_t Budget : {0u, 1u, 2u}) {
    auto R = gatherControlDependencies(State, Value, {}, Budget);
    EXPECT_EQ(R.Status, ControlDiscoveryStatus::BudgetExceeded);
    EXPECT_EQ(R.Visited, Budget);
    EXPECT_TRUE(R.RegisterRanges.empty());
    EXPECT_TRUE(R.FrameSlots.empty());
  }
  auto R = gatherControlDependencies(State, Value, {}, 3);
  EXPECT_EQ(R.Status, ControlDiscoveryStatus::Complete);
  EXPECT_EQ(R.Visited, 3u);
  EXPECT_EQ(R.RegisterRanges.size(), 2u);
}

TEST(ControlDiscovery, SharedExpressionsAreChargedOncePerSlice) {
  SymContext Ctx;
  SymState State(Ctx);
  auto A = State.read(SymSpace::Register, 16, 8);
  auto B = State.read(SymSpace::Register, 32, 8);
  auto Shared = Ctx.mkAdd(A, B);
  auto Value = Ctx.mkOr(Ctx.mkEq(Shared, Ctx.mkConst(64, 2)),
                        Ctx.mkEq(Shared, Ctx.mkConst(64, 5)));
  auto R = gatherControlDependencies(State, Value, {}, 8);
  EXPECT_EQ(R.Status, ControlDiscoveryStatus::Complete);
  EXPECT_EQ(R.Visited, 8u);
  EXPECT_EQ(R.RegisterRanges.size(), 2u);
}

TEST(ControlDiscovery, FrameOffsetNeedsTheExactRoot) {
  SymContext Ctx;
  auto Root = Ctx.mkFreshVar(64, "frame");
  auto Other = Ctx.mkFreshVar(64, "other");
  EXPECT_EQ(frameRelativeOffset(Ctx, Root, Root), 0u);
  EXPECT_EQ(frameRelativeOffset(
                Ctx, Ctx.mkAdd(Root, Ctx.mkConst(64, uint64_t(-32))), Root),
            uint64_t(-32));
  EXPECT_FALSE(frameRelativeOffset(Ctx, Other, Root));
  EXPECT_FALSE(frameRelativeOffset(Ctx, Ctx.mkAdd(Root, Other), Root));
  EXPECT_FALSE(frameRelativeOffset(Ctx, Ctx.mkAdd(Root, Root), Root));
}

TEST(ControlDiscovery, WideningKeepsNarrowLanes) {
  SymContext Ctx;
  SymState State(Ctx);
  auto Byte = State.read(SymSpace::Register, 17, 1);
  auto Sign = Ctx.mkExtract(Ctx.mkSExt(Byte, 64), 56, 8);
  auto R = gatherControlDependencies(State, Sign, {}, 100);
  ASSERT_EQ(R.Status, ControlDiscoveryStatus::Complete);
  ASSERT_EQ(R.RegisterRanges.size(), 1u);
  EXPECT_EQ(R.RegisterRanges[0].Offset, 17u);
  EXPECT_EQ(R.RegisterRanges[0].Bytes, 1u);
  auto Zero = Ctx.mkExtract(Ctx.mkZExt(Byte, 64), 56, 8);
  R = gatherControlDependencies(State, Zero, {}, 100);
  EXPECT_EQ(R.Status, ControlDiscoveryStatus::Complete);
  EXPECT_TRUE(R.RegisterRanges.empty());
}

TEST(ControlDiscovery, AliasingStoreDoesNotReuseTheOldLoadedInput) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto Root = Ctx.mkFreshVar(64, "frame");
  const auto Address = Ctx.mkAdd(Root, Ctx.mkConst(64, uint64_t(-16)));
  const auto Original = State.read(SymSpace::Register, 16, 8);
  State.store(Address, Original);
  const auto Before = State.load(Address, 8);
  auto R = gatherControlDependencies(State, Before, Root, 100);
  ASSERT_EQ(R.RegisterRanges.size(), 1u);
  EXPECT_EQ(R.RegisterRanges[0].Offset, 16u);

  // The external store starts a new unknown epoch for the frame bank. Its
  // reload may nominate the slot again, but cannot recover the old register
  // origin or certify any finite values from it.
  State.store(State.read(SymSpace::Register, 32, 8), Ctx.mkConst(64, 0));
  const auto After = State.load(Address, 8);
  ASSERT_NE(Before, After);
  EXPECT_FALSE(Ctx.asConst(After));
  R = gatherControlDependencies(State, After, Root, 100);
  EXPECT_TRUE(R.RegisterRanges.empty());
  ASSERT_EQ(R.FrameSlots.size(), 1u);
  EXPECT_EQ(R.FrameSlots[0].Offset, -16);
  EXPECT_EQ(R.FrameSlots[0].Bytes, 8u);
}

TEST(ControlDiscovery, NarrowFrameDiscoveryChargesLoadOrigins) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto Root = Ctx.mkFreshVar(64, "frame");
  const auto Loaded = State.load(Root, 8);
  const auto Byte = Ctx.mkExtract(Loaded, 40, 8);
  auto R = gatherControlDependencies(State, Byte, Root, 2);
  EXPECT_EQ(R.Status, ControlDiscoveryStatus::BudgetExceeded);
  EXPECT_EQ(R.Visited, 2u);
  EXPECT_TRUE(R.FrameSlots.empty());
  R = gatherControlDependencies(State, Byte, Root, 3);
  EXPECT_EQ(R.Status, ControlDiscoveryStatus::Complete);
  EXPECT_EQ(R.Visited, 3u);
  ASSERT_EQ(R.FrameSlots.size(), 1u);
  EXPECT_EQ(R.FrameSlots[0].Offset, 5);
  EXPECT_EQ(R.FrameSlots[0].Bytes, 1u);
}

TEST(ControlDiscovery, SelectTracksItsConditionAndOnlyDemandedArmBytes) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto Flag = State.read(SymSpace::Register, 0, 1);
  const auto Word = State.read(SymSpace::Register, 16, 8);
  const auto Select =
      Ctx.mkIte(Ctx.mkNe(Flag, Ctx.mkConst(8, 0)), Word, Ctx.mkConst(64, 0));
  const auto R =
      gatherControlDependencies(State, Ctx.mkExtract(Select, 16, 8), {}, 100);
  EXPECT_EQ(R.Status, ControlDiscoveryStatus::Complete);
  ASSERT_EQ(R.RegisterRanges.size(), 2u);
  EXPECT_EQ(R.RegisterRanges[0].Offset, 0u);
  EXPECT_EQ(R.RegisterRanges[0].Bytes, 1u);
  EXPECT_EQ(R.RegisterRanges[1].Offset, 18u);
  EXPECT_EQ(R.RegisterRanges[1].Bytes, 1u);
}

TEST(ControlDiscovery, ConflictingLoadOriginsKeepAllCandidateDependencies) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto Root = Ctx.mkFreshVar(64, "frame");
  const auto Value = State.read(SymSpace::Register, 16, 8);
  for (uint64_t Offset : {uint64_t(-16), uint64_t(-8)}) {
    const auto Address = Ctx.mkAdd(Root, Ctx.mkConst(64, Offset));
    State.store(Address, Value);
    ASSERT_EQ(State.load(Address, 8), Value);
  }
  ASSERT_EQ(State.loadOrigins(Value).size(), 2u);
  auto R = gatherControlDependencies(State, Value, Root, 100);
  EXPECT_EQ(R.Status, ControlDiscoveryStatus::Complete);
  ASSERT_EQ(R.FrameSlots.size(), 2u);
  EXPECT_EQ(R.FrameSlots[0].Offset, -16);
  EXPECT_EQ(R.FrameSlots[1].Offset, -8);
  ASSERT_EQ(R.RegisterRanges.size(), 1u);
  EXPECT_EQ(R.RegisterRanges[0].Offset, 16u);

  // These are may-dependencies, not a choice of one authoritative occurrence.
  // Historical frame locations remain mere candidates after the external
  // store; only a fresh edge projection could prove their current values.
  const auto External = State.read(SymSpace::Register, 32, 8);
  State.store(External, Value);
  ASSERT_EQ(State.load(External, 8), Value);
  R = gatherControlDependencies(State, Value, Root, 100);
  EXPECT_EQ(R.Status, ControlDiscoveryStatus::UnsupportedOrigin);
  EXPECT_EQ(R.FrameSlots.size(), 2u);
  ASSERT_EQ(R.RegisterRanges.size(), 2u);
  EXPECT_EQ(R.RegisterRanges[0].Offset, 16u);
  EXPECT_EQ(R.RegisterRanges[1].Offset, 32u);
}
