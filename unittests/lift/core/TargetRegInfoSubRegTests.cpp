//===- TargetRegInfoSubRegTests.cpp - Sub-register lookups ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The sub-register queries search an index of the table by narrow view; they
/// must answer exactly as one scan of the table in order does.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"

#include <set>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

using View = std::pair<uint64_t, uint16_t>;

// The queries as one scan of TRI.SubRegs in table order.

bool scanIsSubRegOf(const TargetRegInfo &TRI, View Narrow, View Wide) {
  if (Narrow.first == Wide.first && Narrow.second < Wide.second)
    return true;
  for (const SubRegEntry &E : TRI.SubRegs)
    if (E.WideRegOff == Wide.first && E.WideSize == Wide.second &&
        E.NarrowRegOff == Narrow.first && E.NarrowSize == Narrow.second)
      return true;
  return false;
}

int scanSubRegByteOffset(const TargetRegInfo &TRI, View Narrow, View Wide) {
  if (Narrow.first == Wide.first && Narrow.second < Wide.second)
    return 0;
  for (const SubRegEntry &E : TRI.SubRegs)
    if (E.WideRegOff == Wide.first && E.WideSize == Wide.second &&
        E.NarrowRegOff == Narrow.first && E.NarrowSize == Narrow.second)
      return E.ByteOffset;
  return -1;
}

bool scanWriteZeroExtends(const TargetRegInfo &TRI, View Reg) {
  const uint16_t MaxWidth = TRI.maxRegisterWidth(Reg.first);
  for (const SubRegEntry &E : TRI.SubRegs)
    if (E.NarrowRegOff == Reg.first && E.NarrowSize == Reg.second &&
        E.WideSize <= MaxWidth && E.WriteZeroExtends)
      return true;
  return false;
}

View scanFindWideReg(const TargetRegInfo &TRI, View Reg) {
  View Best = Reg;
  const uint16_t MaxWidth = TRI.maxRegisterWidth(Reg.first);
  for (const SubRegEntry &E : TRI.SubRegs)
    if (E.NarrowRegOff == Reg.first && E.NarrowSize == Reg.second &&
        E.WideSize <= MaxWidth && E.WideSize > Best.second)
      Best = {E.WideRegOff, E.WideSize};
  const bool MayUseLegacyFallback =
      TRI.GeneralRegs.empty() && Reg.first % TRI.FullRegWidth == 0;
  if (Best.second == Reg.second && Reg.second < TRI.FullRegWidth &&
      (TRI.isGeneralReg(Reg.first) || MayUseLegacyFallback))
    Best = {Reg.first, TRI.FullRegWidth};
  return Best;
}

/// Every view the table names, and the views next to them.
std::pair<std::set<View>, std::set<View>> viewsOf(const TargetRegInfo &TRI) {
  std::set<View> Narrow, Wide;
  for (const SubRegEntry &E : TRI.SubRegs) {
    for (uint64_t Offset :
         {E.NarrowRegOff - 1, E.NarrowRegOff, E.NarrowRegOff + 1, E.WideRegOff})
      for (uint16_t Size : {uint16_t(E.NarrowSize - 1), E.NarrowSize,
                            uint16_t(E.NarrowSize + 1), E.WideSize})
        Narrow.insert({Offset, Size});
    Wide.insert({E.WideRegOff, E.WideSize});
    Wide.insert({E.WideRegOff, uint16_t(E.WideSize + 1)});
    Wide.insert({E.NarrowRegOff, E.WideSize});
  }
  return {Narrow, Wide};
}

void expectScanAnswers(const TargetRegInfo &TRI) {
  const auto [Narrow, Wide] = viewsOf(TRI);
  for (const View &N : Narrow) {
    EXPECT_EQ(TRI.findWideReg(N.first, N.second), scanFindWideReg(TRI, N))
        << N.first << ":" << N.second;
    EXPECT_EQ(TRI.writeZeroExtends(N.first, N.second),
              scanWriteZeroExtends(TRI, N))
        << N.first << ":" << N.second;
    for (const View &W : Wide) {
      ASSERT_EQ(TRI.isSubRegOf(N.first, N.second, W.first, W.second),
                scanIsSubRegOf(TRI, N, W))
          << N.first << ":" << N.second << " in " << W.first << ":" << W.second;
      ASSERT_EQ(TRI.subRegByteOffset(N.first, N.second, W.first, W.second),
                scanSubRegByteOffset(TRI, N, W))
          << N.first << ":" << N.second << " in " << W.first << ":" << W.second;
    }
  }
}

TEST(TargetRegInfoSubRegs, IndexedLookupsAnswerAsTheTableScan) {
  for (Arch Target : {Arch::X64, Arch::X86, Arch::AArch64, Arch::ARM}) {
    SCOPED_TRACE(static_cast<int>(Target));
    const TargetRegInfo &TRI = getTargetRegInfo(Target);
    ASSERT_FALSE(TRI.SubRegs.empty());
    ASSERT_NE(TRI.SubRegLookup, nullptr);
    expectScanAnswers(TRI);
  }
}

TEST(TargetRegInfoSubRegs, AnotherTableIsScanned) {
  // Reversed, the table breaks ties between equally wide containers the
  // other way; the index of the original must not answer for it.
  TargetRegInfo TRI = getTargetRegInfo(Arch::X64);
  const std::vector<SubRegEntry> Reversed(TRI.SubRegs.rbegin(),
                                          TRI.SubRegs.rend());
  TRI.SubRegs = Reversed;
  expectScanAnswers(TRI);
}

} // namespace
