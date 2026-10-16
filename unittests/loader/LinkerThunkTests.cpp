//===- LinkerThunkTests.cpp - Thunks a linker writes that only jump on ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/LinkerThunk.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstring>
#include <gtest/gtest.h>
#include <vector>

using namespace neverd;

namespace {

void putWord(std::vector<uint8_t> &Data, size_t Offset, uint32_t Word) {
  std::memcpy(Data.data() + Offset, &Word, sizeof(Word));
}

/// An ELF image of \p A with \p Data as its code at 0x1000.
BinaryImage codeImage(Arch A, std::vector<uint8_t> Data) {
  BinaryImage Image;
  Image.Arch = A;
  Image.Bits = A == Arch::AArch64 ? Bitness::Bits64 : Bitness::Bits32;
  Image.Format = BinaryFormat::ELF;
  Segment Code;
  Code.VA = 0x1000;
  Code.Size = Data.size();
  Code.FileSz = Data.size();
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Data = std::move(Data);
  Image.Segments.push_back(std::move(Code));
  return Image;
}

struct ThunkCase {
  std::vector<uint32_t> Words;
  uint64_t Target;
  bool Thumb;
  uint8_t Size;
};

/// Checks each case's words, placed at 0x1200, against its long branch.
void expectLongBranches(Arch A, InstructionMode Mode,
                        llvm::ArrayRef<ThunkCase> Cases) {
  for (const ThunkCase &Case : Cases) {
    std::vector<uint8_t> Data(0x300, 0);
    for (size_t I = 0; I < Case.Words.size(); ++I)
      putWord(Data, 0x200 + 4 * I, Case.Words[I]);
    const BinaryImage Image = codeImage(A, std::move(Data));
    const std::optional<LinkerThunk> Thunk =
        linkerLongBranchThunkAt(Image, 0x1200, Mode);
    ASSERT_TRUE(Thunk.has_value()) << std::hex << Case.Words.front();
    EXPECT_EQ(Thunk->Target, Case.Target) << std::hex << Case.Words.front();
    EXPECT_EQ(Thunk->Thumb, Case.Thumb) << std::hex << Case.Words.front();
    EXPECT_EQ(Thunk->Size, Case.Size) << std::hex << Case.Words.front();
    EXPECT_FALSE(Thunk->DirectJump);
  }
}

} // namespace

// lld's and GNU ld's A32 long branches: where each goes, in which state, and
// how many bytes it takes.
TEST(LinkerThunk, ARMLongBranchesNameTheirTargetStateAndSize) {
  expectLongBranches(
      Arch::ARM, InstructionMode::ARM,
      {// movw ip, #0x1101; movt ip, #0; bx ip
       {{0xE301C101, 0xE340C000, 0xE12FFF1C}, 0x1100, true, 12},
       // movw ip, #0xfef0; movt ip, #0xffff; add ip, ip, pc; bx ip: pc reads
       // eight bytes past the add at 0x1208.
       {{0xE30FCEF0, 0xE34FCFFF, 0xE08CC00F, 0xE12FFF1C}, 0x1100, false, 16},
       // ldr pc, [pc, #-4]; .word 0x1101
       {{0xE51FF004, 0x1101}, 0x1100, true, 8}});
}

// lld's T32 long branches.  The words hold the halfwords little end first.
TEST(LinkerThunk, ThumbLongBranchesNameTheirTargetStateAndSize) {
  expectLongBranches(Arch::ARM, InstructionMode::Thumb,
                     {// movw ip, #0xfef4; movt ip, #0xffff; add ip, pc; bx ip:
                      // pc reads four bytes past the add at 0x1208.
                      {{0x6CF4F64F, 0x7CFFF6CF, 0x476044FC}, 0x1100, false, 12},
                      // movw ip, #0x1101; movt ip, #0; bx ip
                      {{0x1C01F241, 0x0C00F2C0, 0x4760}, 0x1100, true, 10}});
}

// lld's A64 range thunks, `adrp x16; add x16, x16, #lo12; br x16` and
// `ldr x16, #8; br x16` with the address after them.
TEST(LinkerThunk, AArch64RangeThunksNameTheirTargetAndSize) {
  expectLongBranches(
      Arch::AArch64, InstructionMode::Default,
      {{{0x90000010, 0x91040210, 0xD61F0200}, 0x1100, false, 12},
       {{0x58000050, 0xD61F0200, 0x1100, 0}, 0x1100, false, 16}});
}

// A direct jump is a thunk where a branch enters it, but its form alone, which
// ends many a routine, is no long branch.
TEST(LinkerThunk, ADirectJumpIsNoLongBranch) {
  const struct {
    Arch A;
    InstructionMode Mode;
    uint32_t Word;
  } Jumps[] = {
      // b 0x1100 from 0x1200: A32's pc reads eight bytes past the branch.
      {Arch::ARM, InstructionMode::ARM,
       0xEA000000 | (((0x1100 - 0x1208) >> 2) & 0x00FFFFFF)},
      {Arch::AArch64, InstructionMode::Default,
       0x14000000 | (((0x1100 - 0x1200) >> 2) & 0x03FFFFFF)}};
  for (const auto &Jump : Jumps) {
    std::vector<uint8_t> Data(0x300, 0);
    putWord(Data, 0x200, Jump.Word);
    const BinaryImage Image = codeImage(Jump.A, std::move(Data));
    const std::optional<LinkerThunk> Thunk =
        linkerThunkAt(Image, 0x1200, Jump.Mode);
    ASSERT_TRUE(Thunk.has_value()) << std::hex << Jump.Word;
    EXPECT_TRUE(Thunk->DirectJump);
    EXPECT_EQ(Thunk->Target, 0x1100u);
    EXPECT_EQ(Thunk->Size, 4u);
    EXPECT_FALSE(linkerLongBranchThunkAt(Image, 0x1200, Jump.Mode));
  }
}
