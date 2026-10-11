//===- RegistrationInstructionRangeTests.cpp - PE32 instruction ownership
//--===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reject padding guesses inside instructions without swallowing real entries.
//===----------------------------------------------------------------------===//

#include "../../../lib/loader/COFF/eh/COFFRegistrationEHDetail.h"
#include "gtest/gtest.h"

namespace {
using namespace neverd;
using namespace neverd::coff_loader::registration_detail;

BinaryImage codeImage(std::vector<uint8_t> Bytes) {
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::COFF;
  Image.Entry = 0x1000;
  Segment Text;
  Text.VA = Image.Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = std::move(Bytes);
  Text.Size = Text.FileSz = Text.Data.size();
  Section Code;
  Code.Name = ".text";
  Code.VA = Text.VA;
  Code.Size = Code.FileSz = Text.Size;
  Code.Flags = Text.Flags;
  Image.Sections.push_back(Code);
  Image.Segments.push_back(std::move(Text));
  return Image;
}

TEST(RegistrationInstructionRange,
     BranchesAndCallsCannotSplitAnInstructionOperand) {
  struct Case {
    const char *Name;
    std::vector<uint8_t> Bytes;
    unsigned Boundary;
    bool Disproved;
  };
  const Case Cases[] = {
      {"branch to call",
       {0x85, 0xc0, 0x74, 1, 0x90, 0xe8, 0xcc, 0xe9, 0xfe, 0xff, 0xc3},
       7,
       true},
      {"call then operand",
       {0xe8, 0x40, 0, 0, 0, 0xc7, 0x86, 0xcc, 0x33, 0, 0, 0, 0, 0, 0, 0xc3},
       8,
       true},
      {"branch past return",
       {0xeb, 1, 0xc3, 0xc7, 0x86, 0xcc, 0x33, 0, 0, 0, 0, 0, 0, 0xc3},
       6,
       true},
      {"loop before operand",
       {0x85, 0xc0, 0x75, 0xfc, 0xc7, 0x86, 0xcc, 0x33, 0, 0, 0, 0, 0, 0, 0xc3},
       7,
       true},
      {"return before operand",
       {0xc3, 0xc7, 0x86, 0xcc, 0x33, 0, 0, 0, 0, 0, 0, 0xc3},
       4,
       false},
      {"direct jump operand", {0xeb, 0x40, 0xc3}, 1, true},
      {"indirect jump operand", {0xff, 0xe0, 0xc3}, 1, false},
      {"indirect exit",
       {0xff, 0xe0, 0xc7, 0x86, 0xcc, 0x33, 0, 0, 0, 0, 0, 0, 0xc3},
       5,
       false},
      {"unreachable operand",
       {0xeb, 11, 0x90, 0xc7, 0x86, 0xcc, 0x33, 0, 0, 0, 0, 0, 0, 0xc3},
       6,
       false},
      {"exact entry after call",
       {0xe8, 0x40, 0, 0, 0, 0xc7, 0x86, 0xcc, 0x33, 0, 0, 0, 0, 0, 0, 0xc3},
       5,
       false},
      {"explicit conflicting entry",
       {0x75, 4, 0x90, 0xc7, 0x86, 0xcc, 0x33, 0, 0, 0, 0, 0, 0, 0xc3},
       6,
       false},
      {"overlapping instructions",
       {0x75, 2, 0x90, 0xc7, 0x86, 0xcc, 0x33, 0, 0, 0, 0, 0, 0, 0xc3},
       6,
       false},
  };
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Name);
    auto Image = codeImage(C.Bytes);
    const va_t Boundary = Image.Entry + C.Boundary;
    auto Guess = Symbol::makeFunc(Boundary);
    Guess.IsBoundaryGuess = true;
    Image.Symbols.push_back(Guess);
    size_t Work = 0;
    EXPECT_EQ(disprovesCallbackBoundary(Image, Image.Entry, Boundary, Work),
              C.Disproved);
    const size_t RequiredWork = Work;
    ASSERT_GT(RequiredWork, 0u);
    const auto Range = FunctionRangeMap(Image).find(Image, Image.Entry, true);
    ASSERT_TRUE(Range);
    EXPECT_EQ(Range->End,
              C.Disproved ? Image.Entry + C.Bytes.size() : Boundary);
    // Even an operand-looking address is retained when a symbol or export
    // states an independent entry. Loading cannot revoke that authority.
    Image.Symbols.back().Name = "stated_entry";
    const auto Stated = FunctionRangeMap(Image).find(Image, Image.Entry, true);
    ASSERT_TRUE(Stated);
    EXPECT_EQ(Stated->End, Boundary);
    Work = limits::kMaxRegistrationEHStateWork - RequiredWork + 1;
    EXPECT_FALSE(disprovesCallbackBoundary(Image, Image.Entry, Boundary, Work));
    EXPECT_LE(Work, limits::kMaxRegistrationEHStateWork);
  }
}
} // namespace
