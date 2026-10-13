//===- SEHRuntimeMemoryTests.cpp - Authenticated CRT frame effects --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks authenticated CRT frame effects and invalidated source evidence.

#include "PipelineSEHRuntimeMemory.h"
#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowScalarConstant.h"
#include "neverd/ir/low/SEHFrameProof.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include <algorithm>
#include <map>

using namespace neverd;

namespace {

constexpr va_t Entry = 0x140001000, Leaf = Entry + 0x100;
constexpr va_t Unwind = Entry + 0x200, Format = Entry + 0x1000;
constexpr va_t StreamIAT = Entry + 0x2000, PrintIAT = StreamIAT + 8;

void word(std::vector<uint8_t> &Bytes, uint64_t Value, unsigned Size) {
  for (unsigned I = 0; I != Size; ++I)
    Bytes.push_back(Value >> (I * 8));
}

struct Fixture {
  BinaryImage Image;
  va_t UnwindCall = InvalidVA;
  Fixture(bool Print, bool Overwrite, uint8_t SavedSlot = 40,
          bool OpaqueBeforeSave = false, bool RebindIAT = false,
          bool EscapeFrameBits = false) {
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::COFF;
    Image.Base = Entry - 0x1000;
    Image.Entry = Entry;
    Segment Text;
    Text.Name = ".text";
    Text.VA = Entry;
    Text.Size = 0x201;
    Text.FileSz = Text.Size;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.assign(Text.Size, 0xcc);
    std::vector<uint8_t> Code{0x48, 0x83, 0xec, 0x38}; // sub rsp,56
    const auto DirectCall = [&](va_t Target) {
      Code.push_back(0xe8);
      word(Code, Target - (Entry + Code.size() + 4), 4);
    };
    const auto ImportCall = [&](va_t IAT) {
      Code.insert(Code.end(), {0xff, 0x15});
      word(Code, IAT - (Entry + Code.size() + 4), 4);
    };
    if (OpaqueBeforeSave)
      DirectCall(Unwind);
    Code.insert(Code.end(), {0x48, 0x89, 0x64, 0x24, SavedSlot, 0xb9, 1, 0, 0,
                             0}); // save frame; ecx=1
    if (Overwrite)
      Code.insert(Code.end(), {0x48, 0x8d, 0x54, 0x24, SavedSlot});
    if (Print) {
      ImportCall(StreamIAT);
      Code.insert(Code.end(), {0x48, 0x89, 0xc2,   // FILE
                               0x4c, 0x8d, 0x05}); // RIP-relative format
      word(Code, Format - (Entry + Code.size() + 4), 4);
      Code.insert(Code.end(),
                  {0x45, 0x31, 0xc9, // locale=0
                   0x48, 0x8d, 0x44, 0x24, 0x30, 0x48, 0x89, 0x44, 0x24, 0x20});
      ImportCall(PrintIAT);
    } else
      DirectCall(Leaf);
    Code.insert(Code.end(), {0x48, 0x8b, 0x4c, 0x24, SavedSlot});
    UnwindCall = Entry + Code.size();
    DirectCall(Unwind);
    Code.insert(Code.end(), {0x48, 0x83, 0xc4, 0x38, 0xc3});
    std::copy(Code.begin(), Code.end(), Text.Data.begin());
    std::vector<uint8_t> Body;
    if (RebindIAT) {
      Body.insert(Body.end(), {0x48, 0xb8});
      word(Body, StreamIAT, 8);
      Body.insert(Body.end(), {0x48, 0xb9});
      word(Body, 0, 8);
      Body.insert(Body.end(), {0x48, 0x89, 0x08});
    }
    constexpr va_t EscapedBits = StreamIAT + 0x100;
    if (EscapeFrameBits) {
      Body.insert(Body.end(), {0x48, 0x89, 0xe0,    // mov rax,rsp
                               0x48, 0x83, 0xf0, 8, // xor rax,8
                               0x48, 0xba});
      word(Body, EscapedBits, 8);
      Body.insert(Body.end(), {0x48, 0x89, 0x02});
    }
    if (Overwrite)
      Body.insert(Body.end(), {0x48, 0xc7, 0x02, 0, 0, 0, 0});
    Body.insert(Body.end(), {0x48, 0x83, 0xec, 0x28, 0xff, 0x15});
    word(Body, StreamIAT - (Leaf + Body.size() + 4), 4);
    Body.insert(Body.end(), {0x48, 0x83, 0xc4, 0x28, 0xc3});
    std::copy(Body.begin(), Body.end(), Text.Data.begin() + Leaf - Entry);
    Text.Data[Unwind - Entry] = 0xc3;
    Image.Symbols.push_back(Symbol::makeFunc(Entry, Code.size()));
    Image.Symbols.push_back(Symbol::makeFunc(Leaf, Body.size()));
    Image.Symbols.push_back(Symbol::makeFunc(Unwind, 1));
    Image.Segments.push_back(std::move(Text));
    Segment Data;
    Data.Name = ".rdata";
    Data.VA = Format;
    Data.Flags = SegmentFlags::Readable;
    const std::string Message = "result=%d\n";
    Data.Data.assign(Message.begin(), Message.end());
    Data.Data.push_back(0);
    Data.Size = Data.FileSz = Data.Data.size();
    Image.Segments.push_back(std::move(Data));
    Segment IAT;
    IAT.Name = ".idata";
    IAT.VA = StreamIAT;
    IAT.Size = IAT.FileSz = 16;
    IAT.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    IAT.Data.assign(16, 0);
    Image.Segments.push_back(std::move(IAT));
    if (EscapeFrameBits) {
      Segment Escaped;
      Escaped.Name = ".data";
      Escaped.VA = EscapedBits;
      Escaped.Size = Escaped.FileSz = 8;
      Escaped.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Escaped.Data.assign(8, 0);
      Image.Segments.push_back(std::move(Escaped));
    }
    for (const auto &Segment : Image.Segments) {
      Section Section;
      Section.Name = Segment.Name;
      Section.VA = Segment.VA;
      Section.Size = Segment.Size;
      Section.FileSz = Segment.FileSz;
      Section.FileOff = Segment.FileOff;
      Section.Flags = Segment.Flags;
      Image.Sections.push_back(std::move(Section));
    }
    Image.Imports.push_back(
        {"api-ms-win-crt-stdio-l1-1-0.dll", "__acrt_iob_func", 0, StreamIAT});
    Image.Imports.push_back(
        {"ucrtbase.dll", "__stdio_common_vfprintf", 0, PrintIAT});
    for (const auto &Import : Image.Imports)
      EXPECT_TRUE(Image.recordImportStorageSlot(
          Import.IATAddr, Import.Name, 0,
          ImportStorageEvidence::ImportDirectory));
  }
};

} // namespace

TEST(SEHRuntimeMemory, NestedImportedCallKeepsOnlyAuthenticatedSavedCells) {
  for (unsigned Mutation = 0; Mutation != 14; ++Mutation) {
    SCOPED_TRACE(Mutation);
    Fixture F(false, Mutation == 4, 40, Mutation == 6, Mutation == 7,
              Mutation == 8);
    if (Mutation == 1)
      F.Image.Imports.front().Module = "application.dll";
    if (Mutation == 2)
      F.Image.Imports.front().IsDelayImport = true;
    if (Mutation == 3)
      F.Image.Imports.front().Name = "same_abi_uninspectable";
    if (Mutation == 9) {
      F.Image.Segments.back().Flags =
          F.Image.Segments.back().Flags | SegmentFlags::Executable;
      F.Image.Sections.back().Flags =
          F.Image.Sections.back().Flags | SegmentFlags::Executable;
    }
    if (Mutation == 10)
      F.Image.Segments.back().FileSz = 4;
    if (Mutation == 11)
      F.Image.Imports.push_back(
          {"application.dll", "__acrt_iob_func", 0, StreamIAT});
    if (Mutation == 12)
      F.Image.ImportStorageSlots.clear();
    if (Mutation == 13)
      F.Image.ConflictingImportStorageSlots.insert(StreamIAT);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    CFGBuilder Builder;
    std::vector<LowFunc> Bodies;
    Bodies.push_back(Builder.build(F.Image, Dec, Entry, "caller"));
    Bodies.push_back(Builder.build(F.Image, Dec, Leaf, "leaf"));
    ASSERT_TRUE(Bodies[0].hasCompleteLiftCoverage());
    ASSERT_TRUE(Bodies[1].hasCompleteLiftCoverage());
    std::map<va_t, const LowFunc *> Callees{{Leaf, &Bodies[1]}};
    const auto Memory = collectLowSEHRuntimeMemory(F.Image, Bodies);
    const auto &TRI = getTargetRegInfo(Arch::X64);
    size_t Work = 100000;
    const auto Proof = proveLowSEHFrames(Bodies[0], TRI, Work, &Callees,
                                         Mutation == 5 ? nullptr : &Memory);
    if (Mutation) {
      EXPECT_TRUE(Proof.Calls.empty());
      continue;
    }
    ASSERT_EQ(Proof.Calls.size(), 1u);
    EXPECT_EQ(Proof.Calls.begin()->first.first, F.UnwindCall);
    EXPECT_EQ(Proof.Calls.begin()->second, -56);
    EXPECT_TRUE(Proof.UsesRuntimeMemory);
    EXPECT_EQ(Proof.CalleeDependencies, std::vector<va_t>{Leaf});
    EXPECT_EQ(
        Proof.DependencyDigest,
        lowSEHFrameDependencyDigest(Bodies[0], {Leaf}, &Callees, &Memory));
    EXPECT_NE(Proof.DependencyDigest,
              lowSEHFrameDependencyDigest(Bodies[0], {Leaf}, &Callees));
  }
}

TEST(SEHRuntimeMemory, FormatAndRuntimeHomeAreaOwnTheirActualEffects) {
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    Fixture F(true, false,
              Mutation == 6   ? 24
              : Mutation == 8 ? 0xf8
              : Mutation == 9 ? 0xf0
                              : 40);
    if (Mutation == 1 || Mutation == 2 || Mutation == 3 || Mutation == 7) {
      auto &Data = F.Image.Segments[1];
      const std::string Format = Mutation == 1   ? "%n"
                                 : Mutation == 2 ? "%hhn"
                                 : Mutation == 3 ? "%"
                                                 : "%I64n";
      Data.Data.assign(Format.begin(), Format.end());
      Data.Data.push_back(0);
      Data.Size = Data.FileSz = Data.Data.size();
    }
    if (Mutation == 4)
      F.Image.Segments[1].Flags =
          F.Image.Segments[1].Flags | SegmentFlags::Writable;
    if (Mutation == 5)
      F.Image.Imports.back().Module = "application.dll";
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    CFGBuilder Builder;
    auto Caller = Builder.build(F.Image, Dec, Entry, "caller");
    ASSERT_TRUE(Caller.hasCompleteLiftCoverage());
    auto Memory = collectLowSEHRuntimeMemory(F.Image, {Caller});
    size_t Work = 100000;
    const auto Proof = proveLowSEHFrames(Caller, getTargetRegInfo(Arch::X64),
                                         Work, nullptr, &Memory);
    if (Mutation)
      EXPECT_TRUE(Proof.Calls.empty());
    else {
      ASSERT_EQ(Proof.Calls.size(), 1u);
      EXPECT_EQ(Proof.Calls.begin()->first.first, F.UnwindCall);
      EXPECT_EQ(Proof.Calls.begin()->second, -56);
      EXPECT_TRUE(Proof.UsesRuntimeMemory);
    }
  }
}

TEST(SEHRuntimeMemory, ImportAndFormatChangesInvalidateTheReceipt) {
  Fixture F(true, false);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  CFGBuilder Builder;
  auto Caller = Builder.build(F.Image, Dec, Entry, "caller");
  const auto Original = collectLowSEHRuntimeMemory(F.Image, {Caller});
  const auto Digest =
      lowSEHFrameDependencyDigest(Caller, {}, nullptr, &Original);
  ASSERT_FALSE(Digest.empty());
  F.Image.Imports.back().Module = "application.dll";
  const auto Unbound = collectLowSEHRuntimeMemory(F.Image, {Caller});
  EXPECT_NE(Digest, lowSEHFrameDependencyDigest(Caller, {}, nullptr, &Unbound));
  F.Image.Imports.back().Module = "ucrtbase.dll";
  F.Image.Segments[1].Data[0] = 'R';
  const auto Changed = collectLowSEHRuntimeMemory(F.Image, {Caller});
  EXPECT_NE(Digest, lowSEHFrameDependencyDigest(Caller, {}, nullptr, &Changed));
}

TEST(SEHRuntimeMemory, WritablePEStorageRequiresCompleteUniqueFileBytes) {
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    SCOPED_TRACE(Mutation);
    Fixture F(false, false);
    auto &Segment = F.Image.Segments.back();
    auto &Section = F.Image.Sections.back();
    if (Mutation == 1)
      F.Image.IsRelocatable = true;
    if (Mutation == 2)
      Segment.Flags = SegmentFlags::Readable;
    if (Mutation == 3)
      Section.Flags = SegmentFlags::Readable;
    if (Mutation == 4)
      Segment.ReadOnlyAfterRelocations = true;
    if (Mutation == 5)
      Section.FileSz = 4;
    if (Mutation == 6)
      Segment.FileSz = 4;
    if (Mutation == 7)
      F.Image.Sections.push_back(Section);
    const auto Size = Mutation == 8 ? 0 : 8;
    EXPECT_EQ(isFileBackedWritableImageRange(F.Image, StreamIAT, Size),
              Mutation == 0);
  }
}

TEST(SEHRuntimeMemory, PEImportSlotsRequireDirectoryAndCompleteStorage) {
  for (unsigned Width : {4u, 8u})
    for (unsigned Mutation = 0; Mutation != 20; ++Mutation) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Mutation);
      BinaryImage Image;
      Image.Arch = Width == 4 ? Arch::X86 : Arch::X64;
      Image.Bits = Width == 4 ? Bitness::Bits32 : Bitness::Bits64;
      Image.Format = BinaryFormat::COFF;
      const va_t Base = Mutation == 15 ? 0x140000000 : 0x400000;
      const va_t Slot = Base + 16;
      Segment Storage;
      Storage.VA = Base;
      Storage.Size = Storage.FileSz = 32;
      Storage.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Storage.Data.assign(32, 0);
      Image.Segments.push_back(Storage);
      Section Section;
      Section.VA = Base;
      Section.Size = Section.FileSz = 32;
      Section.Flags = Storage.Flags;
      Image.Sections.push_back(Section);
      Image.Imports.push_back({"application.dll", "routine", 0, Slot});
      ASSERT_TRUE(Image.recordImportStorageSlot(
          Slot, "routine", 0, ImportStorageEvidence::ImportDirectory));
      if (Mutation == 1)
        Image.Segments.front().FileSz = 16 + Width - 1;
      if (Mutation == 2)
        Image.Sections.front().FileSz = 16 + Width - 1;
      if (Mutation == 3)
        Image.Segments.front().Flags = Storage.Flags | SegmentFlags::Executable;
      if (Mutation == 4)
        Image.Sections.front().Flags = Storage.Flags | SegmentFlags::Executable;
      if (Mutation == 5)
        Image.Imports.front().IsDelayImport = true;
      if (Mutation == 6)
        Image.ImportStorageSlots.clear();
      if (Mutation == 7)
        Image.ConflictingImportStorageSlots.insert(Slot);
      if (Mutation == 8)
        Image.ImportStorageSlots[Slot].Addend = 1;
      if (Mutation == 9)
        Image.Imports.push_back({"other.dll", "routine", 0, Slot});
      if (Mutation == 10)
        Image.Imports.push_back({"application.dll", "other_routine", 0, Slot});
      if (Mutation == 11)
        Image.BaseRelocations.push_back({Slot, 3}); // PE HIGHLOW fixup
      if (Mutation == 12)
        Image.DataPtrRelocSlots.insert(Slot);
      if (Mutation == 13)
        Image.Sections.push_back(Section);
      if (Mutation == 14)
        Image.Sections.front().FileOff = 4;
      if (Mutation == 16) {
        Image.Segments.front().Flags = SegmentFlags::Readable;
        Image.Sections.front().Flags = SegmentFlags::Readable;
      }
      if (Mutation == 17)
        Image.Imports.push_back(Image.Imports.front());
      if (Mutation == 18) {
        Image.Imports.push_back(Image.Imports.front());
        Image.Imports.back().IsDelayImport = true;
      }
      if (Mutation == 19)
        Image.IsRelocatable = true;
      const bool Valid = Mutation == 0 || Mutation == 16 || Mutation == 17 ||
                         (Mutation == 15 && Width == 8);
      EXPECT_EQ(isFileBackedPEImportSlot(Image, Slot), Valid);
    }
}

TEST(SEHRuntimeMemory, ScalarAddressesRespectDeclaredWidthsAndUnknownInputs) {
  const auto Constant = [](const NdVar &V) -> std::optional<uint64_t> {
    return V.isConst() ? std::optional<uint64_t>{V.Offset} : std::nullopt;
  };
  LowOp Op;
  Op.Opcode = NdOp::INT_ADD;
  Op.Output = NdVar::tmp(0, 4);
  Op.addInput(NdVar::cst(0xfffffff0, 4));
  Op.addInput(NdVar::cst(0x20, 4));
  EXPECT_EQ(lowScalarConstantResult(Op, Constant), 0x10);
  Op.Opcode = NdOp::INT_SUB;
  Op.Output.Size = Op.Inputs[0].Size = Op.Inputs[1].Size = 1;
  Op.Inputs[0].Offset = 0;
  Op.Inputs[1].Offset = 1;
  EXPECT_EQ(lowScalarConstantResult(Op, Constant), 255);
  Op.Opcode = NdOp::INT_XOR;
  Op.Inputs[0] = Op.Inputs[1] = NdVar::reg(0, 1);
  EXPECT_EQ(lowScalarConstantResult(Op, Constant), 0);
  Op.Inputs[1] = NdVar::reg(8, 1);
  EXPECT_FALSE(lowScalarConstantResult(Op, Constant));
  Op.Opcode = NdOp::INT_ZEXT;
  Op.NumInputs = 1;
  Op.Output.Size = 8;
  Op.Inputs[0] = NdVar::cst(UINT64_MAX, 4);
  EXPECT_EQ(lowScalarConstantResult(Op, Constant), UINT32_MAX);
  Op.Inputs[0].Size = 0;
  EXPECT_FALSE(lowScalarConstantResult(Op, Constant));
  Op.Inputs[0].Size = 8;
  Op.Output.Size = 4;
  EXPECT_FALSE(lowScalarConstantResult(Op, Constant));
  Op.Output.Size = 9;
  EXPECT_FALSE(lowScalarConstantResult(Op, Constant));
  Op.Opcode = NdOp::COPY;
  Op.Output.Size = 4;
  EXPECT_FALSE(lowScalarConstantResult(Op, Constant));
  Op.Opcode = NdOp::LOAD;
  Op.Inputs[0].Size = 4;
  EXPECT_FALSE(lowScalarConstantResult(Op, Constant));
}

TEST(SEHRuntimeMemory, ScalarComparisonsUseTheInputSignAndWidth) {
  const auto Constant = [](const NdVar &V) -> std::optional<uint64_t> {
    return V.isConst() ? std::optional<uint64_t>{V.Offset} : std::nullopt;
  };
  for (uint16_t Width : {1, 2, 4, 8}) {
    SCOPED_TRACE(Width);
    LowOp Op;
    Op.Output = NdVar::tmp(0, 1);
    Op.addInput(NdVar::cst(UINT64_MAX, Width)); // -1 at every input width.
    Op.addInput(NdVar::cst(1, Width));
    Op.Opcode = NdOp::INT_SLESS;
    EXPECT_EQ(lowScalarConstantResult(Op, Constant), 1u);
    Op.Opcode = NdOp::INT_LESS;
    EXPECT_EQ(lowScalarConstantResult(Op, Constant), 0u);
    Op.Inputs[1] = Op.Inputs[0];
    Op.Opcode = NdOp::INT_SLESS;
    EXPECT_EQ(lowScalarConstantResult(Op, Constant), 0u);
    Op.Opcode = NdOp::INT_SLESSEQUAL;
    EXPECT_EQ(lowScalarConstantResult(Op, Constant), 1u);
    Op.Opcode = NdOp::INT_EQUAL;
    EXPECT_EQ(lowScalarConstantResult(Op, Constant), 1u);
    Op.Inputs[1] = NdVar::reg(0, Width);
    EXPECT_FALSE(lowScalarConstantResult(Op, Constant));
    Op.Inputs[1] = NdVar::cst(1, Width == 8 ? 4 : 8);
    EXPECT_FALSE(lowScalarConstantResult(Op, Constant));
    Op.Inputs[1] = NdVar::cst(1, Width);
    Op.Output.Size = 4;
    EXPECT_FALSE(lowScalarConstantResult(Op, Constant));
  }
  LowOp Op;
  Op.Opcode = NdOp::BOOL_NOT;
  Op.Output = NdVar::tmp(0, 1);
  Op.addInput(NdVar::cst(0x100, 1));
  EXPECT_EQ(lowScalarConstantResult(Op, Constant), 1u);
  Op.Inputs[0].Offset = 0x101;
  EXPECT_EQ(lowScalarConstantResult(Op, Constant), 0u);
  Op.Inputs[0].Size = 2;
  EXPECT_FALSE(lowScalarConstantResult(Op, Constant));
}

TEST(SEHRuntimeMemory, RuntimeInvocationCannotOverwriteTheParentReturnPC) {
  for (bool PushOverlaps : {false, true}) {
    SCOPED_TRACE(PushOverlaps);
    Fixture F(true, false);
    auto &Text = F.Image.Segments.front().Data;
    ASSERT_EQ(Text[3], 56);
    Text[3] = 8;
    Text[F.Image.Symbols.front().Size - 2] = 8;
    if (PushOverlaps) {
      // At SP=entry+8 the home area is disjoint, but CALL itself replaces
      // the parent's return PC. SUB after the call cannot repair its value.
      Text[2] = 0xc4;                                // add rsp,8
      Text[F.Image.Symbols.front().Size - 3] = 0xec; // sub rsp,8
    }
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    CFGBuilder Builder;
    auto Caller = Builder.build(F.Image, Dec, Entry, "caller");
    ASSERT_TRUE(Caller.hasCompleteLiftCoverage());
    const auto Memory = collectLowSEHRuntimeMemory(F.Image, {Caller});
    size_t Work = 100000;
    EXPECT_TRUE(proveLowSEHFrames(Caller, getTargetRegInfo(Arch::X64), Work,
                                  nullptr, &Memory)
                    .Calls.empty());
  }
}

TEST(SEHRuntimeMemory, RuntimeVeneersRequireTheirActualIndirectJump) {
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    Fixture F(false, false);
    F.Image.ImportStubIndices[Leaf] = 0;
    auto &Text = F.Image.Segments.front();
    std::vector<uint8_t> Jump{0xff, 0x25};
    word(Jump, StreamIAT - (Leaf + 6), 4);
    std::copy(Jump.begin(), Jump.end(), Text.Data.begin() + Leaf - Entry);
    if (Mutation == 1)
      Text.Data[Leaf - Entry + 1] = 0x15; // CALL instead of JMP
    if (Mutation == 2)
      Text.Data[Leaf - Entry + 2] += 8; // Different import slot
    if (Mutation == 3)
      Text.Flags = Text.Flags | SegmentFlags::Writable;
    if (Mutation == 4)
      F.Image.BaseRelocations.push_back({Leaf + 2, 10}); // DIR64 fixup
    if (Mutation == 5)
      Text.FileSz = Leaf - Entry + 5;
    LowFunc Caller;
    LowBlock Block;
    LowOp Call;
    Call.Opcode = NdOp::CALL;
    Call.addInput(NdVar::cst(Leaf, 8));
    Block.Ops.push_back(Call);
    Caller.Blocks.push_back(Block);
    const auto Memory = collectLowSEHRuntimeMemory(F.Image, {Caller});
    EXPECT_EQ(Memory.calls().count({Leaf, false}), Mutation == 0 ? 1u : 0u);
  }
}
