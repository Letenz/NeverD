//===- RegistrationSEHControlTests.cpp - PE32 near-control evidence -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks physical PE32 control-transfer provenance and malformed encodings.

#include "gtest/gtest.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/STLExtras.h"

using namespace neverd;

TEST(RegistrationSEHControl, UsesActualImmutableNearCallAndReturnBytes) {
  for (unsigned Mutation = 0; Mutation != 13; ++Mutation) {
    SCOPED_TRACE(Mutation);
    BinaryImage Image;
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = BinaryFormat::COFF;
    Image.Base = 0x400000;
    Segment Text;
    Text.VA = 0x401000;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(0x24, 0xcc);
    Text.Data[0] = 0xe8;
    writeLE<uint32_t>(Text.Data.data() + 1, 0x1b);
    Text.Data[0x20] = Mutation == 1 ? 0xc2 : 0xc3;
    if (Mutation == 1) {
      Text.Data[0x21] = 0;
      Text.Data[0x22] = 0;
    }
    Text.Size = Text.FileSz = Text.Data.size();
    Image.Segments.push_back(Text);
    Section Code;
    Code.VA = Text.VA;
    Code.Size = Code.FileSz = Text.Size;
    Code.Flags = Text.Flags;
    Image.Sections.push_back(Code);
    LowFunc F;
    F.Entry = Text.VA;
    F.ExceptionMetadata.emplace();
    F.ExceptionMetadata->CodeRange = {Text.VA, Text.VA + Text.Size};
    F.ExceptionMetadata->Registration.emplace().Scopes = {
        {-1, 0, 0x401020, true}};
    F.Blocks.resize(2);
    auto &Call = F.Blocks[0];
    Call.Id = 0;
    Call.StartAddr = 0x401000;
    Call.EndAddr = 0x401005;
    Call.InstructionBoundaries = {{Call.StartAddr, 5}};
    Call.InstructionBoundaries.back().Control = LowInstructionControl::Call;
    Call.InstructionBoundaries.back().ControlFlags =
        LowInstructionControlFlag::Call;
    Call.InstructionBoundaries.back().Immediate = 0x401020;
    auto &Return = F.Blocks[1];
    Return.Id = 1;
    Return.StartAddr = 0x401020;
    Return.EndAddr = Return.StartAddr + (Mutation == 1 ? 3 : 1);
    Return.InstructionBoundaries = {
        {Return.StartAddr, uint16_t(Return.EndAddr - Return.StartAddr)}};
    Return.InstructionBoundaries.back().Control = LowInstructionControl::Return;
    Return.InstructionBoundaries.back().ControlFlags =
        LowInstructionControlFlag::Return;
    if (Mutation == 1)
      Return.InstructionBoundaries.back().Immediate = 0;
    if (Mutation == 2)
      Image.Segments[0].Data[0] = 0x9a; // Far CALL.
    if (Mutation == 3)
      Image.Segments[0].Data[1] = 0x1c; // Different direct target.
    if (Mutation == 4)
      Image.Segments[0].Data[0x20] = 0xcb; // Far RET.
    if (Mutation == 5)
      Image.Segments[0].Data[0x20] = 0xcf; // IRET.
    if (Mutation == 6)
      Image.Segments[0].Flags =
          Image.Segments[0].Flags | SegmentFlags::Writable;
    if (Mutation == 7)
      Image.Segments[0].FileSz = 0x20;
    if (Mutation == 8)
      Image.Segments.push_back(Image.Segments.front());
    if (Mutation == 9)
      Call.InstructionBoundaries.back().ControlFlags |=
          LowInstructionControlFlag::Conditional;
    if (Mutation == 10)
      Return.InstructionBoundaries.back().Immediate = 4;
    if (Mutation == 11)
      Image.Arch = Arch::X64;
    if (Mutation == 12)
      Call.InstructionBoundaries.back().TargetMode =
          LowInstructionTargetMode::ARM;
    RegistrationCallCalleeIndex Index(Image);
    const auto Controls = Index.sehControlContracts(F);
    ASSERT_TRUE(Controls);
    const auto Has = [&](RegistrationSEHControlContract::Kind Kind) {
      return llvm::any_of(*Controls,
                          [&](const auto &C) { return C.Control == Kind; });
    };
    EXPECT_EQ(Has(RegistrationSEHControlContract::Kind::NearCall),
              Mutation < 2 || Mutation == 4 || Mutation == 5 || Mutation == 7 ||
                  Mutation == 10);
    EXPECT_EQ(Has(RegistrationSEHControlContract::Kind::NearReturn),
              Mutation < 4 || Mutation == 9 || Mutation == 12);
  }
}

TEST(RegistrationSEHControl, AuthenticatesTheWholeFoldedReturnJumpPath) {
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    BinaryImage Image;
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = BinaryFormat::COFF;
    Image.Base = 0x400000;
    Segment Text;
    Text.VA = 0x401000;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(0x24, 0x90);
    Text.Data[0] = 0xe8;
    writeLE<uint32_t>(Text.Data.data() + 1, 0x1b);
    Text.Data[5] = 0xeb;
    Text.Data[6] = 9;
    Text.Data[0x20] = 0xc3;
    if (Mutation == 1) {
      Text.Data[5] = 0xe9;
      writeLE<uint32_t>(Text.Data.data() + 6, 6);
    }
    if (Mutation == 3)
      Text.Data[6] = 10;
    if (Mutation == 4)
      Text.Data[6] = uint8_t(-2);
    if (Mutation == 5)
      Text.Flags = Text.Flags | SegmentFlags::Writable;
    if (Mutation == 6)
      Text.Data[5] = 0xea;
    if (Mutation == 8) {
      Text.Data[0x10] = 0xeb;
      Text.Data[0x11] = 6;
    }
    Text.Size = Text.FileSz = Text.Data.size();
    if (Mutation == 2)
      Text.FileSz = 6;
    Image.Segments.push_back(Text);
    Section Code;
    Code.VA = Text.VA;
    Code.Size = Code.FileSz = Text.Size;
    Code.Flags = Text.Flags;
    Image.Sections.push_back(Code);
    LowFunc F;
    F.Entry = Text.VA;
    F.ExceptionMetadata.emplace();
    F.ExceptionMetadata->CodeRange = {Text.VA, Text.VA + Text.Size};
    F.ExceptionMetadata->Registration.emplace().Scopes = {
        {-1, 0, 0x401020, true}};
    F.Blocks.resize(3);
    auto &Call = F.Blocks[0];
    Call.Id = 0;
    Call.StartAddr = 0x401000;
    Call.EndAddr = 0x401005;
    Call.Succs = Mutation == 7 ? std::vector<int>{1, 2} : std::vector<int>{2};
    Call.InstructionBoundaries = {{Call.StartAddr, 5}};
    Call.InstructionBoundaries.back().Control = LowInstructionControl::Call;
    Call.InstructionBoundaries.back().ControlFlags =
        LowInstructionControlFlag::Call;
    Call.InstructionBoundaries.back().Immediate = 0x401020;
    auto &Return = F.Blocks[1];
    Return.Id = 1;
    Return.StartAddr = 0x401020;
    Return.EndAddr = 0x401021;
    Return.InstructionBoundaries = {{Return.StartAddr, 1}};
    Return.InstructionBoundaries.back().Control = LowInstructionControl::Return;
    Return.InstructionBoundaries.back().ControlFlags =
        LowInstructionControlFlag::Return;
    auto &Resume = F.Blocks[2];
    Resume.Id = 2;
    Resume.StartAddr = Mutation >= 8 ? 0x401018 : 0x401010;
    Resume.EndAddr = Resume.StartAddr + 1;
    RegistrationCallCalleeIndex Index(Image);
    const auto Controls = Index.sehControlContracts(F);
    ASSERT_TRUE(Controls);
    const auto Count = llvm::count_if(*Controls, [](const auto &C) {
      return C.Control == RegistrationSEHControlContract::Kind::NearJump;
    });
    EXPECT_EQ(Count, Mutation == 0 || Mutation == 1 ? 1
                     : Mutation == 8                ? 2
                                                    : 0);
  }
}
