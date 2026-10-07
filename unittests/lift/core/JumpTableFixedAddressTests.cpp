//===- JumpTableFixedAddressTests.cpp - Unrelocated absolute tables ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/LLVMContext.h"

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace neverd;

namespace {
constexpr va_t Entry = 0x401000;
constexpr va_t Dispatch = 0x401008;
constexpr va_t Table = 0x402000;
constexpr va_t Default = 0x401039;
const std::vector<va_t> Targets = {0x40100f, 0x401015, 0x40101b,
                                   Default,  0x401021, 0x401027,
                                   Default,  0x40102d, 0x401033};

/// `switch (c)` over cases 10, 11, 12, 14, 15, 17 and 18 as
/// `gcc -O2 -fno-pie -no-pie` lowers it. The executable is linked at a fixed
/// address, so no relocation names the table or its entries.
BinaryImage fixedAddressSwitch(bool LoadsAtLinkAddress = true) {
  BinaryImage Image;
  Image.Format = BinaryFormat::ELF;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Entry = Entry;
  Image.LoadsAtLinkAddress = LoadsAtLinkAddress;
  Segment Text;
  Text.VA = Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  // sub edi,10; cmp edi,8; ja default; jmp [rdi*8 + 0x402000]
  Text.Data = {0x83, 0xef, 0x0a, 0x83, 0xff, 0x08, 0x77, 0x31,
               0xff, 0x24, 0xfd, 0x00, 0x20, 0x40, 0x00};
  // mov eax, 100 + position; ret
  for (uint8_t Position : {0, 1, 2, 4, 5, 7, 8})
    Text.Data.insert(
        Text.Data.end(),
        {0xb8, static_cast<uint8_t>(100 + Position), 0, 0, 0, 0xc3});
  // default: mov eax, -1; ret
  Text.Data.insert(Text.Data.end(), {0xb8, 0xff, 0xff, 0xff, 0xff, 0xc3});
  Text.Size = Text.FileSz = Text.Data.size();
  Image.Symbols.push_back(Symbol::makeFunc(Text.VA, Text.Size));
  Section Code;
  Code.Name = ".text";
  Code.VA = Text.VA;
  Code.Size = Text.Size;
  Code.Flags = Text.Flags;
  Image.Sections.push_back(Code);
  Image.Segments.push_back(std::move(Text));
  Segment Constants;
  Constants.VA = Table;
  Constants.Flags = SegmentFlags::Readable;
  for (va_t Target : Targets)
    for (unsigned Byte = 0; Byte < 8; ++Byte)
      Constants.Data.push_back(static_cast<uint8_t>(Target >> (8 * Byte)));
  Constants.Size = Constants.FileSz = Constants.Data.size();
  Section ReadOnly;
  ReadOnly.Name = ".rodata";
  ReadOnly.VA = Constants.VA;
  ReadOnly.Size = Constants.Size;
  ReadOnly.Flags = Constants.Flags;
  Image.Sections.push_back(ReadOnly);
  Image.Segments.push_back(std::move(Constants));
  return Image;
}
} // namespace

TEST(JumpTableFixedAddress, UnrelocatedAbsoluteTableResolves) {
  const BinaryImage Image = fixedAddressSwitch();
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Image.Arch));
  CFGBuilder Builder;
  const LowFunc Low = Builder.build(Image, Dec, Entry);
  ASSERT_EQ(Low.JumpTables.size(), 1U);
  EXPECT_EQ(Low.JumpTables.front().InsnAddr, Dispatch);
  EXPECT_EQ(Low.JumpTables.front().Targets, Targets);
  EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
}

TEST(JumpTableFixedAddress, RelocatableImageNeedsARelocation) {
  // The same bytes in an image the loader may move: the displacement is a
  // number until a relocation says otherwise, so the jump stays unresolved.
  const BinaryImage Image = fixedAddressSwitch(/*LoadsAtLinkAddress=*/false);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Image.Arch));
  CFGBuilder Builder;
  const LowFunc Low = Builder.build(Image, Dec, Entry);
  EXPECT_TRUE(Low.JumpTables.empty());
}

TEST(JumpTableFixedAddress, FieldOffsetOfAScaledPointerStaysANumber) {
  // `mov 0x40(,%rdi,8), %rax` reads field 0x40 of a pointer kept shifted
  // right by 3. Even in a fixed image the offset lands in no segment, so it
  // must not become an address the backends would rebase.
  BinaryImage Image = fixedAddressSwitch();
  auto &Text = Image.Segments.front();
  // mov rax, [rdi*8 + 0x40]; ret
  const std::vector<uint8_t> Code = {0x48, 0x8b, 0x04, 0xfd, 0x40,
                                     0x00, 0x00, 0x00, 0xc3};
  std::copy(Code.begin(), Code.end(), Text.Data.begin());
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Image.Arch));
  CFGBuilder Builder;
  const LowFunc Low = Builder.build(Image, Dec, Entry);
  bool SawOffset = false;
  for (const LowBlock &Block : Low.Blocks)
    for (const LowOp &Op : Block.Ops)
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        if (Op.Inputs[I].isConst() && Op.Inputs[I].Offset == 0x40) {
          SawOffset = true;
          EXPECT_EQ(Op.Inputs[I].Provenance, ConstantAddressProvenance::Scalar);
        }
  EXPECT_TRUE(SawOffset);
}

TEST(JumpTableFixedAddress, SwitchPublishesTheSourceCaseOfEachEntry) {
  BinaryImage Image = fixedAddressSwitch();
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  Options.OnlyFunctionEntries = {Entry};
  const auto Result = Pipeline().run(Image, Context, Options);
  ASSERT_TRUE(Result.Success) << Result.Error;
  const auto Func =
      std::find_if(Result.HighFuncs.begin(), Result.HighFuncs.end(),
                   [](const HighFunc &F) { return F.Entry == Entry; });
  ASSERT_NE(Func, Result.HighFuncs.end());
  const auto Labels = Func->SwitchLabelsByJump.find(Dispatch);
  ASSERT_NE(Labels, Func->SwitchLabelsByJump.end());
  EXPECT_EQ(Labels->second.Values,
            (std::vector<uint64_t>{10, 11, 12, 13, 14, 15, 16, 17, 18}));
  EXPECT_EQ(Labels->second.DefaultPosition, 3);
  EXPECT_EQ(Labels->second.SelectorBits, 32U);
}
