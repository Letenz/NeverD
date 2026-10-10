//===- ResolverLaneViewTests.cpp - Register lane cache identity
//------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "jumptable/ResolverLaneView.h"

#include <algorithm>
#include <limits>
#include <random>
#include <vector>

using namespace neverd;
using detail::ResolverLaneView;
using detail::ResolverLaneViews;

TEST(ResolverLaneViews, HighBytesWidthsAndArchitecturesRemainDistinct) {
  ResolverLaneViews X64(getTargetRegInfo(Arch::X64));
  ResolverLaneViews X86(getTargetRegInfo(Arch::X86));
  for (unsigned I = 0; I < 4; ++I) {
    EXPECT_EQ(X64.get(NdVar::reg(0, 4)),
              (ResolverLaneView{VnodeSpace::REG, 0, 8, 0, 4, true}));
    EXPECT_EQ(X64.get(NdVar::reg(1, 1)),
              (ResolverLaneView{VnodeSpace::REG, 0, 8, 1, 1, true}));
    EXPECT_EQ(X86.get(NdVar::reg(1, 1)),
              (ResolverLaneView{VnodeSpace::REG, 0, 4, 1, 1, true}));
    EXPECT_EQ(X64.get(NdVar::reg(0, 1)),
              (ResolverLaneView{VnodeSpace::REG, 0, 8, 0, 1, true}));
    // These two offsets collide in the cache, but name a ZMM lane and RAX.
    EXPECT_EQ(X64.get(NdVar::reg(512, 8)),
              (ResolverLaneView{VnodeSpace::REG, 512, 64, 0, 8, true}));
    EXPECT_EQ(X64.get(NdVar::reg(0, 8)),
              (ResolverLaneView{VnodeSpace::REG, 0, 8, 0, 8, true}));
  }
}

TEST(ResolverLaneViews,
     TemporariesAndNonRegisterInputsCannotReuseRegisterHits) {
  ResolverLaneViews Views(getTargetRegInfo(Arch::X64));
  ASSERT_TRUE(Views.get(NdVar::reg(0, 4)).Valid);
  EXPECT_EQ(Views.get(NdVar::tmp(0, 4)),
            (ResolverLaneView{VnodeSpace::TEMP, 0, 4, 0, 4, true}));
  EXPECT_EQ(Views.get(NdVar::reg(0, 4)),
            (ResolverLaneView{VnodeSpace::REG, 0, 8, 0, 4, true}));
  for (NdVar V :
       {NdVar::cst(0, 4), NdVar::ram(0, 4), NdVar::reg(0, 0), NdVar::tmp(0, 0)})
    EXPECT_FALSE(Views.get(V).Valid);
}

TEST(ResolverLaneViews, EvictionPreservesColdAnswersAcrossArchitecturalViews) {
  std::mt19937 Random(309);
  for (Arch Target : {Arch::X64, Arch::X86, Arch::ARM, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<int>(Target));
    const auto &TRI = getTargetRegInfo(Target);
    std::vector<std::pair<NdVar, ResolverLaneView>> Cases;
    const auto Add = [&](NdVar V) {
      ResolverLaneViews Cold(TRI);
      Cases.emplace_back(V, Cold.get(V));
    };
    for (const auto &E : TRI.SubRegs) {
      Add(NdVar::reg(E.NarrowRegOff, E.NarrowSize));
      Add(NdVar::reg(E.WideRegOff, E.WideSize));
      Add(NdVar::reg(E.NarrowRegOff + 1, E.NarrowSize));
      Add(NdVar::reg(E.NarrowRegOff, E.NarrowSize + 1));
    }
    for (uint64_t Offset :
         {uint64_t{0}, uint64_t{1} << 63, std::numeric_limits<uint64_t>::max()})
      Add(NdVar::reg(Offset, 1));
    ResolverLaneViews Warm(TRI);
    for (unsigned Round = 0; Round < 3; ++Round) {
      std::shuffle(Cases.begin(), Cases.end(), Random);
      for (const auto &[V, Expected] : Cases) {
        ASSERT_EQ(Warm.get(V), Expected) << V.Offset << ':' << V.Size;
        ASSERT_EQ(Warm.get(V), Expected)
            << "repeat " << V.Offset << ':' << V.Size;
      }
    }
  }
}
