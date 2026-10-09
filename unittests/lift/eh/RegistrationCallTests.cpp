//===- RegistrationCallTests.cpp - Checked PE32 call frame boundaries ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <vector>

using namespace neverd;

namespace {
struct ThrowImage {
  static constexpr va_t TextVA = 0x401000;
  static constexpr va_t TableVA = 0x403000;
  static constexpr va_t DataVA = 0x404000;
  static constexpr va_t ImportVA = TextVA + 0x80;
  static constexpr va_t IATVA = DataVA + 0x30;
  static constexpr va_t CallerPCVA = DataVA + 0x20;
  BinaryImage Image;
  va_t CallVA = 0;

  ThrowImage(const std::vector<uint8_t> &Initialize = {0xc7, 0x45, 0xfc, 7, 0,
                                                       0, 0},
             const std::vector<uint8_t> &Extra = {}, int8_t Object = -4) {
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = BinaryFormat::COFF;
    Image.Base = 0x400000;
    Image.Entry = TextVA;
    Segment Text;
    Text.Name = ".text";
    Text.VA = TextVA;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data = {0x55, 0x8b, 0xec, 0x83, 0xec, 4};
    Text.Data.insert(Text.Data.end(), Initialize.begin(), Initialize.end());
    const std::vector<uint8_t> ObservePC = {0x8b, 0x45, 4,    0xa3,
                                            0x20, 0x40, 0x40, 0x00};
    Text.Data.insert(Text.Data.end(), ObservePC.begin(), ObservePC.end());
    Text.Data.insert(Text.Data.end(), Extra.begin(), Extra.end());
    const std::vector<uint8_t> Arguments = {
        0x68, 0x00, 0x30, 0x40, 0x00, 0x8d, 0x4d, uint8_t(Object), 0x51, 0xe8};
    Text.Data.insert(Text.Data.end(), Arguments.begin(), Arguments.end());
    CallVA = TextVA + Text.Data.size() - 1;
    const size_t Displacement = Text.Data.size();
    Text.Data.resize(Text.Data.size() + 4);
    writeLE<uint32_t>(Text.Data.data() + Displacement,
                      uint32_t(ImportVA - (CallVA + 5)));
    Text.Data.insert(Text.Data.end(), {0x90, 0x8b, 0xe5, 0x5d, 0xc3});
    Text.Data.resize(0x88, 0xcc);
    Text.Data[0x80] = 0xff;
    Text.Data[0x81] = 0x25;
    writeLE<uint32_t>(Text.Data.data() + 0x82, IATVA);
    Text.Size = Text.Data.size();
    Image.Segments.push_back(Text);
    Segment Table;
    Table.Name = ".rdata";
    Table.VA = TableVA;
    Table.Flags = SegmentFlags::Readable;
    Table.Data.resize(0x80);
    Table.Size = Table.Data.size();
    writeLE<uint32_t>(Table.Data.data() + 12, TableVA + 0x20);
    writeLE<uint32_t>(Table.Data.data() + 0x20, 1);
    writeLE<uint32_t>(Table.Data.data() + 0x24, TableVA + 0x30);
    writeLE<uint32_t>(Table.Data.data() + 0x30, 1);
    writeLE<uint32_t>(Table.Data.data() + 0x34, DataVA);
    writeLE<uint32_t>(Table.Data.data() + 0x3c, UINT32_MAX);
    writeLE<uint32_t>(Table.Data.data() + 0x44, 4);
    Image.Segments.push_back(Table);
    Segment Data;
    Data.Name = ".data";
    Data.VA = DataVA;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(0x80);
    Data.Data[8] = '.';
    Data.Data[9] = 'H';
    Data.Size = Data.Data.size();
    Image.Segments.push_back(Data);
    Import Import;
    Import.Module = "VCRUNTIME140.dll";
    Import.Name = "_CxxThrowException";
    Import.IATAddr = IATVA;
    Image.Imports.push_back(Import);
  }

  void tableWord(size_t Offset, uint32_t Value) {
    writeLE<uint32_t>(Image.Segments[1].Data.data() + Offset, Value);
  }
};
} // namespace

TEST(RegistrationCallABI, ChecksImmutableSimpleThrowInfo) {
  for (unsigned Mutation = 0; Mutation != 26; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ThrowImage F;
    switch (Mutation) {
    case 1:
      F.tableWord(0, 8);
      break;
    case 2:
      F.tableWord(4, ThrowImage::TextVA);
      break;
    case 3:
      F.tableWord(8, ThrowImage::TextVA);
      break;
    case 4:
      F.tableWord(12, 0);
      break;
    case 5:
      F.tableWord(0x20, 0);
      break;
    case 6:
      F.tableWord(0x20, 2);
      break;
    case 7:
      F.tableWord(0x24, 0);
      break;
    case 8:
      F.tableWord(0x30, 0);
      break;
    case 9:
      F.tableWord(0x30, 5);
      break;
    case 10:
      F.tableWord(0x38, 4);
      break;
    case 11:
      F.tableWord(0x3c, 0);
      break;
    case 12:
      F.tableWord(0x40, 4);
      break;
    case 13:
      F.tableWord(0x44, 0);
      break;
    case 14:
      F.tableWord(0x44, 0x100001);
      break;
    case 15:
      F.tableWord(0x48, ThrowImage::TextVA);
      break;
    case 16:
      F.tableWord(0x34, 0);
      break;
    case 17:
      F.Image.Segments[2].Data[8] = 'H';
      break;
    case 18:
      std::fill(F.Image.Segments[2].Data.begin() + 9,
                F.Image.Segments[2].Data.end(), 'H');
      break;
    case 19:
      F.Image.Segments[1].Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      break;
    case 20:
      F.Image.Segments[1].Flags = SegmentFlags::Readable | SegmentFlags::Executable;
      break;
    case 21:
      F.Image.Arch = Arch::X64;
      break;
    case 22:
      F.Image.Bits = Bitness::Bits64;
      break;
    case 23:
      F.Image.Segments[1].Data.resize(15);
      break;
    case 24:
      // A valid-looking TypeDescriptor cannot overlap its CatchableType.
      F.tableWord(0x34, ThrowImage::TableVA + 0x48);
      F.Image.Segments[1].Data[0x50] = '.';
      F.Image.Segments[1].Data[0x51] = 'H';
      break;
    case 25:
      F.Image.Format = BinaryFormat::ELF;
      break;
    }
    const auto P = coff_loader::getCheckedX86SimpleCxxThrowInfo(
        F.Image, ThrowImage::TableVA);
    ASSERT_EQ(P.has_value(), Mutation == 0);
    if (!P)
      continue;
    EXPECT_EQ(P->Address, ThrowImage::TableVA);
    EXPECT_EQ(P->TypeDescriptorVA, ThrowImage::DataVA);
    EXPECT_EQ(P->ObjectSize, 4u);
    ASSERT_EQ(P->ReadOnlyRanges.size(), 3u);
    EXPECT_EQ(P->ReadOnlyRanges[0].Begin, ThrowImage::TableVA);
    EXPECT_EQ(P->ReadOnlyRanges[0].End, ThrowImage::TableVA + 16);
    EXPECT_EQ(P->TypeDescriptorRange.Begin, ThrowImage::DataVA);
    EXPECT_EQ(P->TypeDescriptorRange.End, ThrowImage::DataVA + 11);
  }
}

TEST(RegistrationCallABI, BindsTheThrowImportAndInitializedPrivateObject) {
  for (unsigned Mutation = 0; Mutation != 13; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ThrowImage F;
    switch (Mutation) {
    case 1:
      F.Image.Imports.front().Module = "untrusted.dll";
      break;
    case 2:
      F.Image.Imports.front().Name = "CxxThrowException";
      break;
    case 3:
      F.Image.Imports.front().IATAddr = 0;
      break;
    case 4:
      F.tableWord(0x20, 2);
      break;
    case 5:
      F = ThrowImage(std::vector<uint8_t>{});
      break;
    case 6:
      F = ThrowImage({0xc6, 0x45, 0xfc, 7});
      break;
    case 7:
      F = ThrowImage({0xc7, 0x45, 0xfc, 7, 0, 0, 0}, {}, -8);
      break;
    case 8:
      F = ThrowImage({0xc7, 0x45, 0xfc, 7, 0, 0, 0}, {}, 8);
      break;
    case 9:
      F = ThrowImage({0x8d, 0x45, 0xfc, 0x89, 0x45, 0xfc});
      break;
    case 10:
      F = ThrowImage({0xc7, 0x45, 0xfc, 7, 0, 0, 0}, {0x64, 0xa1, 0, 0, 0, 0});
      break;
    case 11:
      // Reading the observed real caller PC back would erase its provenance.
      F = ThrowImage({0xc7, 0x45, 0xfc, 7, 0, 0, 0},
                     {0xa1, 0x21, 0x40, 0x40, 0x00});
      break;
    case 12:
      // A runtime type-name mutation cannot hide behind its writable cache.
      F = ThrowImage({0xc7, 0x45, 0xfc, 7, 0, 0, 0},
                     {0xc6, 0x05, 0x08, 0x40, 0x40, 0x00, 'X'});
      break;
    }
    const auto P =
        getCheckedX86RegistrationThrowCalleeABI(F.Image, ThrowImage::TextVA);
    ASSERT_EQ(P.has_value(), Mutation == 0);
    if (!P)
      continue;
    EXPECT_EQ(P->Target, ThrowImage::TextVA);
    EXPECT_EQ(P->ImportVA, ThrowImage::ImportVA);
    EXPECT_EQ(P->ImportIATVA, ThrowImage::IATVA);
    EXPECT_EQ(P->ThrowCallVA, F.CallVA);
    EXPECT_EQ(P->ThrowCallEndVA, F.CallVA + 5);
    EXPECT_GE(P->ThrowOpSeq, 0);
    EXPECT_EQ(P->ObjectOffset, -4);
    EXPECT_EQ(P->ThrowInfo.ObjectSize, 4u);
    ASSERT_EQ(P->CallerPCWrites.size(), 1u);
    EXPECT_EQ(P->CallerPCWrites.front().Begin, ThrowImage::CallerPCVA);
    EXPECT_EQ(P->CallerPCWrites.front().End, ThrowImage::CallerPCVA + 4);
  }
}

TEST(RegistrationCallABI, UsesTheExactScalarObjectWidth) {
  for (uint32_t Width : {1u, 2u, 4u, 8u}) {
    SCOPED_TRACE(Width);
    ThrowImage F({0xc6, 0x45, 0xfc, 7});
    F.tableWord(0x44, Width);
    const auto P =
        getCheckedX86RegistrationThrowCalleeABI(F.Image, ThrowImage::TextVA);
    EXPECT_EQ(P.has_value(), Width == 1);
    if (P)
      EXPECT_EQ(P->ThrowInfo.ObjectSize, Width);
  }
}

TEST(RegistrationCallABI, SharesTheBudgetAcrossFailedAndSuccessfulProofs) {
  ThrowImage F;
  size_t Work = 0;
  EXPECT_FALSE(getCheckedX86RegistrationLeafCalleeABI(
      F.Image, ThrowImage::TextVA, &Work));
  EXPECT_GT(Work, 0u);
  const size_t FailedWork = Work;
  ASSERT_TRUE(getCheckedX86RegistrationThrowCalleeABI(
      F.Image, ThrowImage::TextVA, &Work));
  EXPECT_GT(Work, FailedWork);
  Work = limits::kMaxRegistrationEHStateWork;
  EXPECT_FALSE(getCheckedX86RegistrationLeafCalleeABI(
      F.Image, ThrowImage::TextVA, &Work));
  EXPECT_FALSE(getCheckedX86RegistrationThrowCalleeABI(
      F.Image, ThrowImage::TextVA, &Work));
  EXPECT_EQ(Work, limits::kMaxRegistrationEHStateWork);
}

TEST(RegistrationCallABI, BindsTheMemoizedContractToOneBuildImage) {
  ThrowImage F;
  LowFunc Caller;
  Caller.Blocks.resize(1);
  LowOp Call;
  Call.Opcode = NdOp::CALL;
  Call.addInput(NdVar::cst(ThrowImage::TextVA, 4));
  Caller.Blocks[0].Ops = {Call, Call};
  RegistrationCallCalleeIndex Index(F.Image);
  const auto Contracts = Index.contracts(Caller);
  ASSERT_TRUE(Contracts);
  ASSERT_EQ(Contracts->size(), 1u);
  const auto &C = Contracts->front();
  EXPECT_EQ(C.CalleeKind, RegistrationCalleeFrameContract::Kind::PrivateThrow);
  EXPECT_TRUE(C.DoesNotReturn);
  EXPECT_EQ(C.ThrownTypeVA, ThrowImage::DataVA);
  EXPECT_EQ(C.ThrownObjectSize, 4u);
  EXPECT_TRUE(C.ECXReads.empty());
  EXPECT_TRUE(C.ECXWrites.empty());
  ASSERT_EQ(C.CallerPCWrites.size(), 1u);
  EXPECT_EQ(C.CallerPCWrites[0].Begin, ThrowImage::CallerPCVA);
  EXPECT_EQ(Index.contracts(Caller)->size(), 1u);
  F.Image.Imports[0].Name = "unknown_throw";
  RegistrationCallCalleeIndex NextBuild(F.Image);
  const auto Changed = NextBuild.contracts(Caller);
  ASSERT_TRUE(Changed);
  EXPECT_TRUE(Changed->empty());
}
