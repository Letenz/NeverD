//===- SymRegionWrapTests.cpp - Modular symbolic region coordinates -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Distinguish a relative coordinate crossing zero from absolute overflow.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymExec.h"

#include <set>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

SymRef at(SymContext &C, SymRef Base, uint64_t Offset) {
  return C.mkAdd(Base, C.mkConst(64, Offset));
}

TEST(SymRegionWrap, CompleteWritesAndPartialOverwritesKeepBothSides) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big})
    for (uint16_t Bytes : {2, 4, 8, 16}) {
      SymContext C;
      SymState S(C, Order);
      const auto Base = C.mkVar("object", 64);
      const uint64_t Offset = uint64_t{0} - Bytes / 2;
      llvm::APInt Value(Bytes * 8, 0);
      std::vector<uint8_t> Expected;
      for (unsigned I = 0; I != Bytes; ++I) {
        const auto Byte = static_cast<uint8_t>(0x31 + 7 * I);
        Expected.push_back(Byte);
        const unsigned Lane =
            Order == llvm::endianness::little ? I : Bytes - I - 1;
        Value |= llvm::APInt(Bytes * 8, Byte).shl(Lane * 8);
      }
      EXPECT_FALSE(S.store(at(C, Base, Offset), C.mkConst(Value)));
      const auto Whole = S.load(at(C, Base, Offset), Bytes);
      ASSERT_TRUE(Whole);
      EXPECT_EQ(C.asConst(Whole), Value);
      for (unsigned I = 0; I != Bytes; ++I)
        EXPECT_EQ(S.load(at(C, Base, Offset + I), 1),
                  C.mkConst(8, Expected[I]));

      // One byte on each side changes, with every other lane preserved.
      S.store(at(C, Base, UINT64_MAX), C.mkConst(8, 0xa3));
      S.store(Base, C.mkConst(8, 0x5c));
      Expected[Bytes / 2 - 1] = 0xa3;
      Expected[Bytes / 2] = 0x5c;
      const auto Changed = S.load(at(C, Base, Offset), Bytes);
      ASSERT_TRUE(Changed);
      for (unsigned I = 0; I != Bytes; ++I) {
        const unsigned Lane =
            Order == llvm::endianness::little ? I : Bytes - I - 1;
        EXPECT_EQ(C.mkExtract(Changed, Lane * 8, 8), C.mkConst(8, Expected[I]));
      }
    }
}

TEST(SymRegionWrap, UntouchedReadRetainsByteIdentityAndNonwrappingOrigins) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext C;
    SymState S(C, Order);
    const auto Base = C.mkVar("object", 64);
    const auto Address = at(C, Base, UINT64_MAX - 2);
    const auto Whole = S.load(Address, 8);
    ASSERT_TRUE(Whole);
    EXPECT_EQ(S.load(Address, 8), Whole);
    for (unsigned I = 0; I != 8; ++I) {
      const unsigned Lane = Order == llvm::endianness::little ? I : 7 - I;
      EXPECT_EQ(S.load(at(C, Base, UINT64_MAX - 2 + I), 1),
                C.mkExtract(Whole, Lane * 8, 8));
    }
    const auto *Load = S.loadOrigin(Whole);
    ASSERT_NE(Load, nullptr);
    EXPECT_EQ(Load->Address, Address);
    EXPECT_EQ(Load->Bytes, 8);
    std::set<SymRef> Seen;
    std::vector<SymRef> Work{Whole};
    std::set<std::pair<uint64_t, uint16_t>> Origins;
    while (!Work.empty()) {
      const auto Current = Work.back();
      Work.pop_back();
      if (!Seen.insert(Current).second)
        continue;
      for (const auto &Origin : S.memoryInputOrigins(Current)) {
        EXPECT_EQ(Origin.Base, Base);
        Origins.emplace(Origin.Offset, Origin.Bytes);
      }
      for (auto Child : C.operands(Current))
        Work.push_back(Child);
    }
    EXPECT_EQ(Origins, (std::set<std::pair<uint64_t, uint16_t>>{
                           {UINT64_MAX - 2, 3}, {0, 5}}));
  }
}

TEST(SymRegionWrap, CrossRegionHavocAndCopiedEpochsRemainConservative) {
  SymContext C;
  SymState S(C);
  const auto Base = C.mkVar("object", 64);
  const auto Other = C.mkVar("other", 64);
  const auto Address = at(C, Base, UINT64_MAX);
  S.store(Address, C.mkConst(64, 0x1122334455667788));
  SymState Before = S;
  S.store(Other, C.mkConst(8, 9));
  const auto After = S.load(Address, 8);
  ASSERT_TRUE(After);
  EXPECT_FALSE(C.isConst(After));
  EXPECT_EQ(Before.load(Address, 8), C.mkConst(64, 0x1122334455667788));
  SymState Copy = S;
  EXPECT_EQ(Copy.load(Address, 8), After);
  S.store(Base, C.mkConst(8, 0x73));
  EXPECT_EQ(Copy.load(Address, 8), After);
  EXPECT_EQ(C.mkExtract(S.load(Address, 8), 8, 8), C.mkConst(8, 0x73));
}

TEST(SymRegionWrap, UnseenRegionCopiesShareUnknownBytesAcrossZero) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext C;
    SymState S(C, Order);
    const auto Base = C.mkVar("unseen", 64);
    S.clobberMemory();
    SymState Copy = S;
    const auto Whole = S.load(at(C, Base, UINT64_MAX - 2), 8);
    ASSERT_TRUE(Whole);
    const auto First = Copy.load(at(C, Base, UINT64_MAX - 2), 3);
    const auto Second = Copy.load(Base, 5);
    EXPECT_EQ(
        First,
        C.mkExtract(Whole, Order == llvm::endianness::little ? 0 : 40, 24));
    EXPECT_EQ(
        Second,
        C.mkExtract(Whole, Order == llvm::endianness::little ? 24 : 0, 40));
    EXPECT_TRUE(S.mergeIdentical(Copy));
  }
}

TEST(SymRegionWrap, PreservationUsesExactBudgetAndDoesNotMutateOnShortBudget) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext C;
    SymState S(C, Order);
    const auto Root = C.mkVar("frame", 64);
    const auto Base = C.mkVar("object", 64);
    S.store(at(C, Root, -24), C.mkConst(64, 0x1234));
    S.store(at(C, Root, -16), C.mkConst(64, 0x5678));
    S.store(at(C, Root, -8), C.mkConst(64, 0x9abc));
    const SymStorePreservationRange More[] = {{-8, 0}};
    SymStorePreservation P{Root, -24, -16, UINT64_MAX};
    P.AdditionalRanges = More;
    const SymState Initial = S;
    const auto Address = at(C, Base, UINT64_MAX - 2);
    EXPECT_FALSE(S.store(Address, C.mkConst(64, 0x1122334455667788), P));
    ASSERT_EQ(P.Status, SymStorePreservationStatus::Applied);
    // One extra range, 24 inspected bytes, one ordered-range cursor advance
    // and 16 bytes each saved and restored.
    ASSERT_EQ(P.WorkUsed, 1u + 24u + 1u + 2u * 16u);
    const auto Required = P.WorkUsed;
    SymState Short = Initial;
    P.MaxWork = Required - 1;
    EXPECT_FALSE(Short.store(Address, C.mkConst(64, 0x1122334455667788), P));
    EXPECT_EQ(P.Status, SymStorePreservationStatus::BudgetExceeded);
    EXPECT_TRUE(Short.mergeIdentical(Initial));
    P.MaxWork = Required;
    EXPECT_FALSE(Short.store(Address, C.mkConst(64, 0x1122334455667788), P));
    ASSERT_EQ(P.Status, SymStorePreservationStatus::Applied);
    EXPECT_EQ(P.WorkUsed, Required);
    EXPECT_EQ(Short.load(at(C, Root, -24), 8), C.mkConst(64, 0x1234));
    EXPECT_FALSE(C.isConst(Short.load(at(C, Root, -16), 8)));
    EXPECT_EQ(Short.load(at(C, Root, -8), 8), C.mkConst(64, 0x9abc));
    EXPECT_EQ(Short.load(Address, 8), C.mkConst(64, 0x1122334455667788));
  }
}

TEST(SymRegionWrap, AbsoluteAndScalarOverflowStillRefuse) {
  SymContext C;
  SymState S(C);
  const auto Root = C.mkVar("frame", 64);
  // A preservation descriptor cannot grant wrapping to an absolute bank,
  // including when its protected region has never been materialized.
  const SymState Empty = S;
  SymStorePreservation P{Root, 0, 8, 100};
  EXPECT_FALSE(S.store(C.mkConst(64, UINT64_MAX), C.mkConst(16, 7), P));
  EXPECT_EQ(P.Status, SymStorePreservationStatus::Invalid);
  EXPECT_TRUE(S.mergeIdentical(Empty));
  S.store(Root, C.mkConst(64, 0x1234));
  const SymState Before = S;
  EXPECT_FALSE(S.store(C.mkConst(64, UINT64_MAX), C.mkConst(16, 7), P));
  EXPECT_EQ(P.Status, SymStorePreservationStatus::Invalid);
  EXPECT_TRUE(S.mergeIdentical(Before));
  for (auto Space :
       {SymSpace::Register, SymSpace::Temporary, SymSpace::Memory}) {
    S.write(Space, UINT64_MAX, C.mkConst(8, 0x3a));
    S.write(Space, UINT64_MAX, C.mkConst(16, 0x1122));
    EXPECT_FALSE(S.read(Space, UINT64_MAX, 2));
    EXPECT_EQ(S.read(Space, UINT64_MAX, 1), C.mkConst(8, 0x3a));
  }
  EXPECT_FALSE(S.load(C.mkConst(64, UINT64_MAX), 2));
  const auto Live = S.numLiveBytes();
  EXPECT_FALSE(S.load(at(C, Root, UINT64_MAX), 0));
  EXPECT_EQ(S.numLiveBytes(), Live);
  P = {Root, 0, 8, 100};
  EXPECT_FALSE(S.store(C.mkVar("narrow", 32), C.mkConst(64, 3), P));
  EXPECT_EQ(P.Status, SymStorePreservationStatus::Invalid);
  P = {Root, 0, 8, 100};
  EXPECT_FALSE(
      S.store(at(C, C.mkVar("object", 64), UINT64_MAX), C.mkConst(7, 3), P));
  EXPECT_EQ(P.Status, SymStorePreservationStatus::Invalid);
  EXPECT_FALSE(S.store(at(C, C.mkVar("object", 64), UINT64_MAX),
                       C.mkConst(65536 * 8, 3), P));
  EXPECT_EQ(P.Status, SymStorePreservationStatus::Invalid);
}

} // namespace
