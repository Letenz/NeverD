#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Support/Endian.h"

using namespace neverd;

namespace {
struct ChainedValueFixture {
  static constexpr va_t Slot = 0x2000;
  static constexpr uint64_t Bits = UINT64_C(0x8000000000003040);
  BinaryImage Image;

  ChainedValueFixture() {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.MachOHasChainedFixups = true;
    for (const va_t Address : {Slot, va_t(0x3000)}) {
      Segment Segment;
      Segment.Name = "__DATA_CONST";
      Segment.VA = Address;
      Segment.Size = Segment.FileSz = 0x100;
      Segment.FileOff = Address - Slot;
      Segment.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Segment.ReadOnlyAfterRelocations = true;
      Segment.Data.resize(Segment.Size);
      Image.Segments.push_back(Segment);
      Section Section;
      Section.Name = "__const";
      Section.SegmentName = Segment.Name;
      Section.VA = Address;
      Section.Size = Section.FileSz = Segment.Size;
      Section.FileOff = Segment.FileOff;
      Section.Flags = Segment.Flags;
      Image.Sections.push_back(Section);
    }
    llvm::support::endian::write64le(Image.Segments[0].Data.data(), Bits);
    Image.MachOResolvedChainedPointerSlots.insert(Slot);
  }
};
} // namespace

TEST(ImmutablePEPointer, RequiresExactRelocationAndUnambiguousStorage) {
  for (unsigned Width : {4u, 8u}) {
    for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Mutation);
      ChainedValueFixture F;
      F.Image.Format = BinaryFormat::COFF;
      F.Image.Arch = Width == 8 ? Arch::X64 : Arch::X86;
      F.Image.Bits = Width == 8 ? Bitness::Bits64 : Bitness::Bits32;
      F.Image.MachOHasChainedFixups = false;
      F.Image.MachOResolvedChainedPointerSlots.clear();
      for (auto &Seg : F.Image.Segments) {
        Seg.Flags = SegmentFlags::Readable;
        Seg.ReadOnlyAfterRelocations = false;
      }
      for (auto &Sec : F.Image.Sections)
        Sec.Flags = SegmentFlags::Readable;
      llvm::support::endian::write64le(F.Image.Segments[0].Data.data(), 0x3040);
      const auto Type = static_cast<uint8_t>(
          Width == 8 ? llvm::COFF::IMAGE_REL_BASED_DIR64
                     : llvm::COFF::IMAGE_REL_BASED_HIGHLOW);
      F.Image.BaseRelocations.push_back({F.Slot, Type});
      F.Image.DataPtrRelocSlots.insert(F.Slot);
      F.Image.DataPtrRelocTargetOwners[F.Slot] = 0x3000;
      switch (Mutation) {
      case 1:
        F.Image.BaseRelocations.clear();
        break;
      case 2:
        F.Image.BaseRelocations.push_back({F.Slot, Type});
        break;
      case 3:
        F.Image.BaseRelocations.push_back({F.Slot + 1, Type});
        break;
      case 4:
        F.Image.BaseRelocations[0].Type = llvm::COFF::IMAGE_REL_BASED_HIGH;
        break;
      case 5:
        F.Image.Segments[0].Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
        break;
      case 6:
        F.Image.Sections[0].FileSz = Width - 1;
        break;
      case 7:
        F.Image.Sections.push_back(F.Image.Sections.front());
        break;
      case 8:
        F.Image.CodePtrRelocSlots.insert(F.Slot);
        break;
      case 9:
        F.Image.DataPtrRelocTargetOwners[F.Slot] = 0x2000;
        break;
      case 10:
        F.Image.ConflictingImportStorageSlots.insert(F.Slot);
        break;
      case 11:
        // Adjacent HIGHLOW slots do not overlap an exact four-byte read.
        F.Image.BaseRelocations.push_back({F.Slot + Width, Type});
        F.Image.DataPtrRelocSlots.insert(F.Slot + Width);
        F.Image.DataPtrRelocTargetOwners[F.Slot + Width] = 0x3000;
        break;
      }
      auto Target = readImmutableImagePointer(F.Image, F.Slot);
      if (Mutation == 0 || Mutation == 11)
        EXPECT_EQ(Target, 0x3040u);
      else
        EXPECT_FALSE(Target);
      EXPECT_FALSE(readImmutableImageBytes(F.Image, F.Slot, Width));
    }
  }
}

TEST(ImmutablePEPointer, ScalarReadsRespectTheTargetPointerWidth) {
  for (Arch Architecture : {Arch::X86, Arch::ARM, Arch::X64, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<unsigned>(Architecture));
    const unsigned Width =
        Architecture == Arch::X86 || Architecture == Arch::ARM ? 4 : 8;
    ChainedValueFixture F;
    F.Image.Format = BinaryFormat::COFF;
    F.Image.Arch = Architecture;
    F.Image.Bits = Width == 4 ? Bitness::Bits32 : Bitness::Bits64;
    F.Image.MachOHasChainedFixups = false;
    F.Image.MachOResolvedChainedPointerSlots.clear();
    llvm::support::endian::write64le(F.Image.Segments[0].Data.data(), 0x3040);
    F.Image.DataPtrRelocSlots.insert(F.Slot);
    F.Image.DataPtrRelocTargetOwners[F.Slot] = 0x3000;
    const auto Type =
        static_cast<uint8_t>(Width == 4 ? llvm::COFF::IMAGE_REL_BASED_HIGHLOW
                                        : llvm::COFF::IMAGE_REL_BASED_DIR64);
    F.Image.BaseRelocations.push_back({F.Slot, Type});
    // The following null sentinel is disjoint from the complete pointer slot.
    const auto Sentinel =
        readImmutableImageBytes(F.Image, F.Slot + Width, Width);
    ASSERT_TRUE(Sentinel);
    EXPECT_EQ(*Sentinel, std::vector<uint8_t>(Width, 0));
    for (unsigned Offset = 0; Offset < Width; ++Offset)
      EXPECT_FALSE(readImmutableImageBytes(F.Image, F.Slot + Offset, Width));
    EXPECT_FALSE(readImmutableImageBytes(F.Image, F.Slot + Width - 1, 1));
    EXPECT_TRUE(readImmutableImageBytes(F.Image, F.Slot + Width, 1));
  }
}

TEST(ImmutablePEPointer, CodeTargetsNeedImmutableAlignedInstructionStorage) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::AArch64}) {
    for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
      SCOPED_TRACE(static_cast<unsigned>(Architecture));
      SCOPED_TRACE(Mutation);
      ChainedValueFixture F;
      const unsigned Width = Architecture == Arch::X86 ? 4 : 8;
      F.Image.Arch = Architecture;
      F.Image.Bits = Width == 4 ? Bitness::Bits32 : Bitness::Bits64;
      F.Image.Format = BinaryFormat::COFF;
      F.Image.MachOHasChainedFixups = false;
      F.Image.MachOResolvedChainedPointerSlots.clear();
      const va_t Target = Mutation == 3 ? 0x3041 : 0x3040;
      llvm::support::endian::write64le(F.Image.Segments[0].Data.data(), Target);
      F.Image.BaseRelocations.push_back(
          {F.Slot, static_cast<uint8_t>(
                       Width == 4 ? llvm::COFF::IMAGE_REL_BASED_HIGHLOW
                                  : llvm::COFF::IMAGE_REL_BASED_DIR64)});
      F.Image.CodePtrRelocSlots.insert(F.Slot);
      auto &Text = F.Image.Segments[1];
      Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
      Text.ReadOnlyAfterRelocations = false;
      F.Image.Sections[1].Flags = Text.Flags;
      F.Image.Symbols.push_back(Symbol::makeFunc(Target));
      if (Mutation == 1)
        Text.Flags = Text.Flags | SegmentFlags::Writable;
      if (Mutation == 2) {
        Text.FileSz = Architecture == Arch::AArch64 ? 0x43 : 0x40;
        F.Image.Sections[1].FileSz = Text.FileSz;
      }
      if (Mutation == 4)
        F.Image.Symbols.clear();
      const bool Accepted =
          Mutation == 0 || (Mutation == 3 && Architecture != Arch::AArch64);
      EXPECT_EQ(readImmutableImageCodePointer(F.Image, F.Slot).has_value(),
                Accepted);
    }
  }
}

TEST(ImmutableImageBytes, ARMLiteralReadsStayInsideTheirDataIsland) {
  for (BinaryFormat Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
    SCOPED_TRACE(static_cast<unsigned>(Format));
    ChainedValueFixture F;
    F.Image.Arch = Arch::ARM;
    F.Image.Bits = Bitness::Bits32;
    F.Image.Format = Format;
    F.Image.MachOHasChainedFixups = false;
    F.Image.MachOResolvedChainedPointerSlots.clear();
    F.Image.Segments[0].Flags =
        SegmentFlags::Readable | SegmentFlags::Executable;
    F.Image.Sections[0].Flags = F.Image.Segments[0].Flags;
    F.Image.Sections[0].Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
    F.Image.ARMCodeRegions = {
        {F.Slot, F.Slot + 4, ARMCodeRegionKind::Data},
        {F.Slot + 4, F.Slot + 0x100, ARMCodeRegionKind::ARM}};
    EXPECT_TRUE(readImmutableImageBytes(F.Image, F.Slot, 4));
    EXPECT_TRUE(readImmutableImageBytes(F.Image, F.Slot + 3, 1));
    EXPECT_FALSE(readImmutableImageBytes(F.Image, F.Slot, 8));
    EXPECT_FALSE(readImmutableImageBytes(F.Image, F.Slot + 3, 2));
    EXPECT_FALSE(readImmutableImageBytes(F.Image, F.Slot + 4, 4));
  }
}

TEST(ImmutableChainedValue,
     ReturnsRuntimeBitsWithoutPointerOrByteCopyAuthority) {
  for (const Arch Architecture : {Arch::AArch64, Arch::X64}) {
    ChainedValueFixture F;
    F.Image.Arch = Architecture;
    EXPECT_EQ(readImmutableChainedImageValue(F.Image, F.Slot), F.Bits);
    EXPECT_FALSE(readInitialImagePointer(F.Image, F.Slot));
    EXPECT_FALSE(readImmutableImagePointer(F.Image, F.Slot));
    EXPECT_FALSE(readImmutableImageBytes(F.Image, F.Slot, 8));
    EXPECT_FALSE(F.Image.isDataAddress(F.Bits));
    EXPECT_FALSE(F.Image.isCodeAddress(F.Bits));
    EXPECT_TRUE(F.Image.isDataAddress(0x3040));
  }
}

TEST(ImmutableChainedValue, RequiresExactCurrentResolvedChainIdentity) {
  for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ChainedValueFixture F;
    switch (Mutation) {
    case 0:
      F.Image.MachOHasChainedFixups = false;
      break;
    case 1:
      F.Image.MachOResolvedChainedPointerSlots.clear();
      break;
    case 2:
      F.Image.MachOChainedFixupsAmbiguous = true;
      break;
    case 3:
      F.Image.MachOResolvedChainedPointerSlots = {F.Slot + 4};
      break;
    case 4:
      F.Image.MachOResolvedChainedPointerSlots.clear();
      F.Image.DataPtrRelocSlots.insert(F.Slot);
      F.Image.DataPtrRelocTargetOwners[F.Slot] = 0x3000;
      break;
    case 5:
      F.Image.Format = BinaryFormat::ELF;
      break;
    case 6:
      F.Image.Bits = Bitness::Bits32;
      break;
    case 7:
      F.Image.IsRelocatable = true;
      break;
    }
    EXPECT_FALSE(readImmutableChainedImageValue(F.Image, F.Slot));
  }
}

TEST(ImmutableChainedValue, RejectsAmbiguousStorageAndCompetingFixups) {
  for (unsigned Mutation = 0; Mutation < 22; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ChainedValueFixture F;
    switch (Mutation) {
    case 0:
      F.Image.Segments[0].ReadOnlyAfterRelocations = false;
      break;
    case 1:
      F.Image.Sections.push_back(F.Image.Sections[0]);
      break;
    case 2:
      F.Image.Segments.push_back(F.Image.Segments[0]);
      break;
    case 3:
      F.Image.Sections[0].FileSz = 7;
      break;
    case 4:
      F.Image.Segments[0].FileSz = 7;
      break;
    case 5:
      F.Image.Sections[0].Size = 7;
      break;
    case 6:
      F.Image.Sections[0].FileOff = 1;
      break;
    case 7:
      F.Image.Sections[0].Type = llvm::MachO::S_ZEROFILL;
      break;
    case 8:
      F.Image.Sections[0].Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
      F.Image.Sections[0].Flags =
          SegmentFlags::Readable | SegmentFlags::Executable;
      break;
    case 9:
      F.Image.DyldBindSlots[F.Slot] = {"_foreign", 0, "foreign", false};
      break;
    case 10:
      F.Image.ConflictingImportStorageSlots.insert(F.Slot);
      break;
    case 11:
      F.Image.ImportPtrSlots[F.Slot] = "_foreign";
      break;
    case 12:
      F.Image.CodePtrRelocSlots.insert(F.Slot);
      break;
    case 13:
      F.Image.DataPtrRelocSlots.insert(F.Slot + 4);
      break;
    case 14:
      F.Image.MachOResolvedChainedPointerSlots.insert(F.Slot + 4);
      break;
    case 15:
      F.Image.RelDataPtrRelocSlots.insert(F.Slot);
      break;
    case 16:
      F.Image.RelCodeRelocSlots.insert(F.Slot);
      break;
    case 17: {
      RelocationEntry R;
      R.Address = F.Slot;
      F.Image.Relocations.push_back(R);
      break;
    }
    case 18: {
      BaseRelocation R;
      R.Address = F.Slot;
      F.Image.BaseRelocations.push_back(R);
      break;
    }
    case 19:
      F.Image.DataPtrRelocTargetOwners[F.Slot + 4] = 0x3000;
      break;
    case 20:
      F.Image.Sections[0].Flags = SegmentFlags::None;
      break;
    case 21:
      F.Image.Segments[0].Flags = SegmentFlags::None;
      break;
    }
    EXPECT_FALSE(readImmutableChainedImageValue(F.Image, F.Slot));
  }
}
