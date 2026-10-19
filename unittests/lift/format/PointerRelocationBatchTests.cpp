//===- PointerRelocationBatchTests.cpp - Absolute pointer relocations -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFLoaderUtils.h"
#include "neverd/loader/PointerRelocation.h"

#include <cstring>
#include <map>
#include <set>
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

TEST(PointerRelocationBatch, PointersToDataInCodeBecomePointersToData) {
  // A kernel keeps string literals and tables in its executable PAGE.  The
  // loader takes a pointer in .data to them for a pointer to code until the
  // debug information places its target in data.
  BinaryImage Img;
  Img.Format = BinaryFormat::COFF;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  const SegmentFlags Code = SegmentFlags::Readable | SegmentFlags::Executable;
  const SegmentFlags Writable = SegmentFlags::Readable | SegmentFlags::Writable;
  addSegment(Img, 0x1000, 0x100, Code);
  addOwner(Img, "PAGE", 0x1000, 0x100, Code);
  addSegment(Img, 0x3000, 0x100, Writable);
  addOwner(Img, ".data", 0x3000, 0x100, Writable);
  Symbol Routine;
  Routine.Name = "routine";
  Routine.Addr = 0x1000;
  Routine.Size = 0x20;
  Routine.IsFunc = true;
  Img.Symbols.push_back(Routine);

  // Inside the routine; a named string; an unnamed table after it; bytes
  // nothing describes.
  const std::vector<AbsolutePointerRelocation> Pointers = {
      {0x3000, 0x1010, InvalidVA},
      {0x3008, 0x1040, InvalidVA},
      {0x3010, 0x1060, InvalidVA},
      {0x3018, 0x1080, InvalidVA}};
  for (const AbsolutePointerRelocation &Pointer : Pointers) {
    const uint64_t Value = Pointer.TargetVA;
    std::memcpy(Img.Segments[1].Data.data() + (Pointer.SlotVA - 0x3000), &Value,
                sizeof(Value));
  }
  recordAbsolutePointerRelocations(Img, Pointers);
  ASSERT_EQ(Img.CodePtrRelocSlots,
            (std::set<va_t>{0x3000, 0x3008, 0x3010, 0x3018}));

  // The debug information places even the routine's interior in data; the
  // routine's own symbol keeps it code.
  const std::set<va_t> InData = {0x1010, 0x1040, 0x1060};
  const auto Places = [&](va_t Addr) { return InData.count(Addr) != 0; };
  EXPECT_EQ(recordDataPointerTargets(Img, Places), 2u);
  EXPECT_EQ(Img.CodePtrRelocSlots, (std::set<va_t>{0x3000, 0x3018}));
  EXPECT_EQ(Img.DataPtrRelocSlots, (std::set<va_t>{0x3008, 0x3010}));
  EXPECT_EQ(Img.DataPtrRelocTargetOwners.at(0x3008), 0x1000u);
  EXPECT_EQ(Img.DataPtrRelocTargetOwners.at(0x3010), 0x1000u);
  EXPECT_TRUE(Img.RelocDataAddrs.count(0x1040));
  EXPECT_TRUE(Img.RelocDataAddrs.count(0x1060));
  EXPECT_TRUE(Img.WritableRelocDataAddrs.empty());
  // Done once, it is done.
  EXPECT_EQ(recordDataPointerTargets(Img, Places), 0u);
}

namespace {

/// A COFF image of \p A with .text at 0x1000 and .data at 0x3000, whose
/// absolute pointer relocations \p Pointers are recorded as the base
/// relocations are.
BinaryImage makeCodeRelocationImage(
    Arch A, const std::vector<AbsolutePointerRelocation> &Pointers) {
  BinaryImage Img;
  Img.Format = BinaryFormat::COFF;
  Img.Arch = A;
  Img.Bits = A == Arch::X64 ? Bitness::Bits64 : Bitness::Bits32;
  const SegmentFlags Code = SegmentFlags::Readable | SegmentFlags::Executable;
  const SegmentFlags Writable = SegmentFlags::Readable | SegmentFlags::Writable;
  addSegment(Img, 0x1000, 0x100, Code);
  addOwner(Img, ".text", 0x1000, 0x100, Code);
  addSegment(Img, 0x3000, 0x100, Writable);
  addOwner(Img, ".data", 0x3000, 0x100, Writable);
  const uint32_t Width = Img.getPointerSize();
  for (const AbsolutePointerRelocation &Pointer : Pointers) {
    const uint64_t Value = Pointer.TargetVA;
    std::memcpy(Img.Segments[0].Data.data() + (Pointer.SlotVA - 0x1000), &Value,
                Width);
  }
  recordAbsolutePointerRelocations(Img, Pointers);
  return Img;
}

} // namespace

TEST(PointerRelocationBatch, PointerRunsAmongCodeAreData) {
  // MSVC places an i386 switch's jump table in .text after its function.  A
  // base relocation in code is an instruction's operand, but no instruction
  // holds three relocated four-byte fields back to back: `mov dword ptr
  // [disp32], imm32` holds two, and the next instruction starts with its
  // opcode.
  //
  // `jmp [eax*4+0x1042]`'s displacement; the table; the two fields of
  // `mov dword ptr [0x3000], 0x1010`.
  BinaryImage Img =
      makeCodeRelocationImage(Arch::X86, {{0x100E, 0x1042, InvalidVA},
                                          {0x1042, 0x1010, InvalidVA},
                                          {0x1046, 0x1018, InvalidVA},
                                          {0x104A, 0x1020, InvalidVA},
                                          {0x1062, 0x3000, InvalidVA},
                                          {0x1066, 0x1010, InvalidVA}});
  ASSERT_TRUE(Img.CodePtrRelocSlots.empty());
  ASSERT_EQ(Img.CodeAddressRelocOperands.size(), 5u);

  coff_loader::recordRelocatedPointerRunsInCode(Img);
  EXPECT_EQ(Img.CodePtrRelocSlots, (std::set<va_t>{0x1042, 0x1046, 0x104A}));
  EXPECT_EQ(Img.DataInCode, (std::set<va_t>{0x1042}));
  for (va_t Slot : {0x1042, 0x1046, 0x104A})
    EXPECT_FALSE(Img.CodeAddressRelocOperands.count(Slot)) << std::hex << Slot;
  // The displacement names the table, which is data; an operand naming data
  // in data's role.
  EXPECT_FALSE(Img.CodeAddressRelocOperands.count(0x100E));
  ASSERT_TRUE(Img.DataAddressRelocOperands.count(0x100E));
  EXPECT_EQ(Img.DataAddressRelocOperands.at(0x100E).TargetVA, 0x1042u);
  // One instruction's two fields stay its operands.
  EXPECT_TRUE(Img.DataAddressRelocOperands.count(0x1062));
  EXPECT_TRUE(Img.CodeAddressRelocOperands.count(0x1066));

  // Done once, it is done.
  const auto Slots = Img.CodePtrRelocSlots;
  const auto CodeOperands = Img.CodeAddressRelocOperands;
  const auto DataOperands = Img.DataAddressRelocOperands;
  coff_loader::recordRelocatedPointerRunsInCode(Img);
  EXPECT_EQ(Img.CodePtrRelocSlots, Slots);
  EXPECT_EQ(Img.CodeAddressRelocOperands.size(), CodeOperands.size());
  EXPECT_EQ(Img.DataAddressRelocOperands.size(), DataOperands.size());
}

TEST(PointerRelocationBatch, TwoAdjacentEightBytePointersAreDataAmongX64Code) {
  // Only x86-64's `mov r64, imm64` and moffs64 forms hold an eight-byte
  // field, one each.
  BinaryImage Img =
      makeCodeRelocationImage(Arch::X64, {{0x1040, 0x1010, InvalidVA},
                                          {0x1048, 0x1018, InvalidVA},
                                          {0x1062, 0x1010, InvalidVA}});
  coff_loader::recordRelocatedPointerRunsInCode(Img);
  EXPECT_EQ(Img.CodePtrRelocSlots, (std::set<va_t>{0x1040, 0x1048}));
  EXPECT_EQ(Img.CodeAddressRelocOperands.size(), 1u);
  EXPECT_TRUE(Img.CodeAddressRelocOperands.count(0x1062));
}

TEST(PointerRelocationBatch, PointerRunsAmongCodeToDataBecomePointersToData) {
  // A kernel also keeps tables of pointers to its literals among its code in
  // PAGE.  A run of them is data whose pointers lead to code until the debug
  // information places a target in data; that pointer then leads to data,
  // and it is no instruction's operand for its section's sake.
  BinaryImage Img = makeCodeRelocationImage(
      Arch::X64, {{0x1080, 0x1040, InvalidVA}, {0x1088, 0x1060, InvalidVA}});
  coff_loader::recordRelocatedPointerRunsInCode(Img);
  ASSERT_EQ(Img.CodePtrRelocSlots, (std::set<va_t>{0x1080, 0x1088}));

  const auto Places = [](va_t Addr) { return Addr == 0x1040; };
  EXPECT_EQ(recordDataPointerTargets(Img, Places), 1u);
  EXPECT_EQ(Img.CodePtrRelocSlots, (std::set<va_t>{0x1088}));
  EXPECT_EQ(Img.DataPtrRelocSlots, (std::set<va_t>{0x1080}));
  EXPECT_EQ(Img.DataPtrRelocTargetOwners.at(0x1080), 0x1000u);
  EXPECT_TRUE(Img.RelocDataAddrs.count(0x1040));
  EXPECT_FALSE(Img.CodeAddressRelocOperands.count(0x1080));
  EXPECT_FALSE(Img.DataAddressRelocOperands.count(0x1080));
  // Done once, it is done.
  EXPECT_EQ(recordDataPointerTargets(Img, Places), 0u);
}

TEST(PointerRelocationBatch, PointerRunsAmongOtherCodeStayOperands) {
  // An ARM literal pool's relocated words are what its loads read; nothing
  // here bounds how many sit together.  An object file's relocations are its
  // own records.
  const std::vector<AbsolutePointerRelocation> Run = {
      {0x1040, 0x1010, InvalidVA},
      {0x1044, 0x1018, InvalidVA},
      {0x1048, 0x1020, InvalidVA}};
  BinaryImage ARM = makeCodeRelocationImage(Arch::ARM, Run);
  coff_loader::recordRelocatedPointerRunsInCode(ARM);
  EXPECT_TRUE(ARM.CodePtrRelocSlots.empty());
  EXPECT_EQ(ARM.CodeAddressRelocOperands.size(), 3u);

  BinaryImage Object = makeCodeRelocationImage(Arch::X86, Run);
  Object.IsRelocatable = true;
  coff_loader::recordRelocatedPointerRunsInCode(Object);
  EXPECT_TRUE(Object.CodePtrRelocSlots.empty());
  EXPECT_EQ(Object.CodeAddressRelocOperands.size(), 3u);
}
