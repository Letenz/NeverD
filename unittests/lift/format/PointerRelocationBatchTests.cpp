//===- PointerRelocationBatchTests.cpp - Absolute pointer relocations -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/PointerRelocation.h"

#include <map>
#include <string>
#include <vector>

using namespace neverd;

namespace {

void addOwner(BinaryImage &Img, llvm::StringRef Name, va_t VA, uint64_t Size,
              SegmentFlags Flags) {
  Section Sec;
  Sec.Name = Name.str();
  Sec.VA = VA;
  Sec.Size = Size;
  Sec.FileSz = Size;
  Sec.Flags = Flags;
  Sec.Data.assign(Size, 0);
  Img.Sections.push_back(std::move(Sec));
}

void addSegment(BinaryImage &Img, va_t VA, uint64_t Size, SegmentFlags Flags) {
  Segment Seg;
  Seg.VA = VA;
  Seg.Size = Size;
  Seg.FileSz = Size;
  Seg.Flags = Flags;
  Seg.Data.assign(Size, 0);
  Img.Segments.push_back(std::move(Seg));
}

/// An image with code, read-only data, writable data, an import slot, a
/// code segment whose tail no section holds (one named routine in it), and a
/// data segment whose tail no section holds.  Some slots already hold
/// relocation records.
BinaryImage makeImage(Arch A, InstructionMode Mode) {
  BinaryImage Img;
  Img.Format = BinaryFormat::COFF;
  Img.Arch = A;
  Img.Mode = Mode;
  Img.Bits = Bitness::Bits32;
  const SegmentFlags Code = SegmentFlags::Readable | SegmentFlags::Executable;
  const SegmentFlags ReadOnly = SegmentFlags::Readable;
  const SegmentFlags Writable = SegmentFlags::Readable | SegmentFlags::Writable;
  addSegment(Img, 0x1000, 0x200, Code);
  addOwner(Img, ".text", 0x1000, 0x100, Code);
  addSegment(Img, 0x2000, 0x100, ReadOnly);
  addOwner(Img, ".rdata", 0x2000, 0x100, ReadOnly);
  addSegment(Img, 0x3000, 0x100, Writable);
  addOwner(Img, ".data", 0x3000, 0x100, Writable);
  addSegment(Img, 0x4000, 0x200, ReadOnly);
  addOwner(Img, ".idata", 0x4000, 0x80, ReadOnly);

  Symbol Routine;
  Routine.Name = "in_the_gap";
  Routine.Addr = 0x1180;
  Routine.IsFunc = true;
  Img.Symbols.push_back(Routine);

  EXPECT_TRUE(Img.recordImportStorageSlot(
      0x2080, "imported", 0, ImportStorageEvidence::ImportDirectory));

  // An operand the relocation of 0x1010 names again, with a flag it does not
  // set, and records the relocations below replace or remove.
  RelocatedAddressField Kept{0x1050, 0x1050, 4, 0x1000};
  Kept.PCRelativeFromInstructionEnd = true;
  Img.CodeAddressRelocOperands[0x1010] = Kept;
  Img.DataAddressRelocOperands[0x1010] = {0x2000, 0x2000, 4, 0x2000};
  Img.CodePtrRelocSlots.insert(0x3000);
  Img.DataPtrRelocSlots.insert(0x2020);
  Img.DataPtrRelocTargetOwners[0x2020] = 0x2000;
  return Img;
}

/// Relocations of every kind, out of order, with slots relocated twice.
std::vector<AbsolutePointerRelocation> relocations() {
  return {
      {0x3008, 0x1070, InvalidVA}, // data slot, code: then data below
      {0x1010, 0x1050, InvalidVA}, // code operand naming code
      {0x1014, 0x2010, InvalidVA}, // code operand naming data
      {0x2020, 0x1060, InvalidVA}, // data slot holding code
      {0x3000, 0x3050, InvalidVA}, // data slot holding writable data
      {0x3004, 0x3100, 0x30FF},    // one past the end of its owner
      {0x3008, 0x2030, InvalidVA}, // the same slot again, now data
      {0x1018, 0x2040, InvalidVA}, // code operand naming data, then code
      {0x1018, 0x1080, InvalidVA},
      {0x2080, 0x1000, InvalidVA}, // an import slot: nothing
      {0x9000, 0x1000, InvalidVA}, // unmapped: nothing
      {0x1190, 0x3010, InvalidVA}, // code segment, no section
      {0x4100, 0x1180, InvalidVA}, // data segment, no section, to code
      {0x4010, 0x11A0, InvalidVA}, // to code segment bytes that are no code
      {0x1020, 0x1100, InvalidVA}, // code one past the end of its owner
      {0x2024, 0x2010, InvalidVA}, // data slot, read-only data
      {0x2028, 0x2010, InvalidVA}, // the same target from another slot
  };
}

bool sameField(const RelocatedAddressField &A, const RelocatedAddressField &B) {
  return A.EncodedValue == B.EncodedValue && A.TargetVA == B.TargetVA &&
         A.Width == B.Width && A.TargetOwnerVA == B.TargetOwnerVA &&
         A.PCRelativeFromInstructionEnd == B.PCRelativeFromInstructionEnd &&
         A.Kind == B.Kind;
}

void expectSameOperands(const std::map<va_t, RelocatedAddressField> &A,
                        const std::map<va_t, RelocatedAddressField> &B) {
  ASSERT_EQ(A.size(), B.size());
  for (auto ItA = A.begin(), ItB = B.begin(); ItA != A.end(); ++ItA, ++ItB) {
    EXPECT_EQ(ItA->first, ItB->first);
    EXPECT_TRUE(sameField(ItA->second, ItB->second)) << ItA->first;
  }
}

void expectBatchMatchesEachInTurn(Arch A, InstructionMode Mode) {
  BinaryImage EachInTurn = makeImage(A, Mode);
  for (const AbsolutePointerRelocation &R : relocations())
    recordAbsolutePointerRelocation(EachInTurn, R.SlotVA, R.TargetVA,
                                    R.TargetOwnerVA);
  BinaryImage Batch = makeImage(A, Mode);
  recordAbsolutePointerRelocations(Batch, relocations());

  EXPECT_EQ(EachInTurn.RelocDataAddrs, Batch.RelocDataAddrs);
  EXPECT_EQ(EachInTurn.WritableRelocDataAddrs, Batch.WritableRelocDataAddrs);
  EXPECT_EQ(EachInTurn.CodePtrRelocSlots, Batch.CodePtrRelocSlots);
  EXPECT_EQ(EachInTurn.DataPtrRelocSlots, Batch.DataPtrRelocSlots);
  EXPECT_EQ(EachInTurn.DataPtrRelocTargetOwners,
            Batch.DataPtrRelocTargetOwners);
  EXPECT_EQ(EachInTurn.CodeRefTargets, Batch.CodeRefTargets);
  expectSameOperands(EachInTurn.CodeAddressRelocOperands,
                     Batch.CodeAddressRelocOperands);
  expectSameOperands(EachInTurn.DataAddressRelocOperands,
                     Batch.DataAddressRelocOperands);

  // The records the relocations speak for, spelled out.
  EXPECT_TRUE(
      Batch.CodeAddressRelocOperands.at(0x1010).PCRelativeFromInstructionEnd);
  EXPECT_FALSE(Batch.DataAddressRelocOperands.count(0x1010));
  EXPECT_TRUE(Batch.CodeAddressRelocOperands.count(0x1018));
  EXPECT_FALSE(Batch.DataAddressRelocOperands.count(0x1018));
  EXPECT_TRUE(Batch.DataPtrRelocSlots.count(0x3000));
  EXPECT_FALSE(Batch.CodePtrRelocSlots.count(0x3000));
  EXPECT_TRUE(Batch.CodePtrRelocSlots.count(0x2020));
  EXPECT_FALSE(Batch.DataPtrRelocSlots.count(0x2020));
  EXPECT_TRUE(Batch.DataPtrRelocSlots.count(0x3008));
  EXPECT_FALSE(Batch.CodePtrRelocSlots.count(0x3008));
  EXPECT_FALSE(Batch.CodePtrRelocSlots.count(0x2080));
  EXPECT_TRUE(Batch.WritableRelocDataAddrs.count(0x3100));
}

} // namespace

TEST(PointerRelocationBatch, RecordsWhatEachRelocationInTurnWould) {
  expectBatchMatchesEachInTurn(Arch::X86, InstructionMode::Default);
}

TEST(PointerRelocationBatch, RecordsWhatEachRelocationInTurnWouldForThumb) {
  // No address of an ARM image is classified by its section alone.
  expectBatchMatchesEachInTurn(Arch::ARM, InstructionMode::Thumb);
}
