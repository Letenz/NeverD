//===- MedStackDeltaTests.cpp - Stack pointer offsets through joins -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The entry-relative stack pointer offset of a value behind a long chain of
/// joins, each merging two arms that adjust the stack pointer alike.

#include "../../../lib/ir/med/abi/MedABIPassDetail.h"
#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/lift/X86/X86Regs.h"

#include <cstdint>
#include <optional>
#include <utility>

namespace {
using namespace neverd;

MedVar rsp(int Version) {
  MedVar Value;
  Value.Kind = MedVar::Reg;
  Value.Id = 1;
  Value.SSAVer = Version;
  Value.RegOff = x86reg::RSP;
  Value.Size = 8;
  Value.TheArch = Arch::X64;
  return Value;
}

// Appends \p Levels joins in a row to \p F. Each join merges two arms that
// subtract \p Left and \p Right from the previous join's stack pointer; the
// first arms start from the live-in stack pointer. Returns the last join.
MedVar joinChain(MedFunc &F, int Levels, std::pair<int64_t, int64_t> Arms) {
  F.Entry = 0x1000;
  F.Blocks.emplace_back().Id = 0;
  MedVar Previous = rsp(0);
  int Version = 1;
  for (int Level = 0; Level < Levels; ++Level) {
    const int Join = 3 * Level + 3;
    PhiNode Phi;
    Phi.Output = rsp(Version++);
    for (int Arm = 0; Arm < 2; ++Arm) {
      MedBlock Block;
      Block.Id = Join - 2 + Arm;
      Block.Preds = {Join - 3};
      Block.Succs = {Join};
      MedOp Step;
      Step.Opcode = NdOp::INT_SUB;
      Step.Output = rsp(Version++);
      Step.addInput(Previous);
      Step.addInput(MedVar::makeConst(
          static_cast<uint64_t>(Arm ? Arms.second : Arms.first), 8));
      Phi.Args.emplace_back(Block.Id, Step.Output);
      Block.Ops.push_back(Step);
      F.Blocks.push_back(std::move(Block));
    }
    MedBlock Merge;
    Merge.Id = Join;
    Merge.Preds = {Join - 2, Join - 1};
    Merge.Phis.push_back(Phi);
    F.Blocks.push_back(std::move(Merge));
    Previous = Phi.Output;
  }
  return Previous;
}

std::optional<int64_t> deltaOf(const MedFunc &F, const MedVar &V,
                               bool Indexed) {
  MedDefSiteIndex Defs(F);
  return stackPtrDelta(F, getTargetRegInfo(Arch::X64), V, 0,
                       Indexed ? &Defs : nullptr);
}

// Every path through 64 joins reaches the live-in stack pointer at the
// depth limit. There are 2^64 of them; the answer must not walk each one.
TEST(MedStackDelta, ResolvesEachJoinOfAChainOnce) {
  for (const bool Indexed : {false, true}) {
    MedFunc F;
    const MedVar Last = joinChain(F, 64, {16, 16});
    EXPECT_EQ(deltaOf(F, Last, Indexed), std::optional<int64_t>(-1024))
        << "indexed=" << Indexed;
  }
}

// One more join puts the live-in stack pointer past the depth limit.
TEST(MedStackDelta, KeepsTheDepthLimitOfAJoinChain) {
  for (const bool Indexed : {false, true}) {
    MedFunc F;
    const MedVar Last = joinChain(F, 65, {16, 16});
    EXPECT_EQ(deltaOf(F, Last, Indexed), std::nullopt) << "indexed=" << Indexed;
  }
}

// Arms that adjust the stack pointer differently leave no single offset.
TEST(MedStackDelta, RefusesArmsWithDifferentOffsets) {
  for (const bool Indexed : {false, true}) {
    MedFunc F;
    const MedVar Last = joinChain(F, 40, {16, 8});
    EXPECT_EQ(deltaOf(F, Last, Indexed), std::nullopt) << "indexed=" << Indexed;
  }
}

TEST(MedStackDelta, SharedAnswersPreserveTheDepthOfEachQuery) {
  for (const int FirstDepth : {0, 1}) {
    MedFunc F;
    const MedVar Last = joinChain(F, 64, {16, 16});
    MedDefSiteIndex Defs(F);
    const auto &TRI = getTargetRegInfo(Arch::X64);
    const auto Expected = [](int Depth) -> std::optional<int64_t> {
      return Depth == 0 ? std::optional<int64_t>(-1024) : std::nullopt;
    };
    EXPECT_EQ(stackPtrDelta(F, TRI, Last, FirstDepth, &Defs),
              Expected(FirstDepth));
    for (const int Depth : {0, 1, 0, 1})
      EXPECT_EQ(stackPtrDelta(F, TRI, Last, Depth, &Defs), Expected(Depth));
  }
}

TEST(MedStackDelta, InvalidationRetiresAnswersWithTheirDefinitions) {
  MedFunc F;
  const MedVar Last = joinChain(F, 3, {16, 16});
  MedDefSiteIndex Defs(F);
  const auto &TRI = getTargetRegInfo(Arch::X64);
  EXPECT_EQ(stackPtrDelta(F, TRI, Last, 0, &Defs), std::optional<int64_t>(-48));
  // One changed arm no longer agrees with the other path through the join.
  F.Blocks[1].Ops[0].Inputs[1] = MedVar::makeConst(8, 8);
  Defs.invalidate();
  EXPECT_EQ(stackPtrDelta(F, TRI, Last, 0, &Defs), std::nullopt);
}

TEST(MedStackDelta, SharedAnswersKeepTheirStackPointerReference) {
  MedFunc F;
  const MedVar Last = joinChain(F, 3, {16, 16});
  MedDefSiteIndex Defs(F);
  const auto &X64 = getTargetRegInfo(Arch::X64);
  const auto &A64 = getTargetRegInfo(Arch::AArch64);
  ASSERT_NE(X64.StackPointer, A64.StackPointer);
  for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
    EXPECT_EQ(stackPtrDelta(F, X64, Last, 0, &Defs),
              std::optional<int64_t>(-48));
    EXPECT_EQ(stackPtrDelta(F, A64, Last, 0, &Defs), std::nullopt);
  }
}

} // namespace
