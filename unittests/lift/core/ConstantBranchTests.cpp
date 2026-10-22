//===- ConstantBranchTests.cpp - Exact scalar branch recovery ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independently authored constant branches with unreachable decode decoys.

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/Support/Error.h"

#include <set>
#include <vector>

using namespace neverd;

namespace {

LowFunc build(std::vector<uint8_t> Bytes, Arch Architecture = Arch::X64,
              BinaryFormat Format = BinaryFormat::ELF) {
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Bits = Architecture == Arch::X86 ? Bitness::Bits32 : Bitness::Bits64;
  Image.Format = Format;
  Image.Base = 0x1000;
  Segment Text;
  Text.VA = Image.Base;
  Text.Size = Bytes.size();
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = std::move(Bytes);
  Image.Segments.push_back(std::move(Text));
  Decoder Dec;
  if (!Dec.init(Image)) {
    ADD_FAILURE() << "decoder initialization failed";
    return {};
  }
  CFGBuilder Builder;
  auto Function = Builder.build(Image, Dec, Image.Base, "constant_branch");
  auto Error = validateLowInstructionBoundaries(
      Function, LowInstructionBoundaryRequirement::Required);
  EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  return Function;
}

std::set<va_t> addresses(const LowFunc &Function) {
  std::set<va_t> Result;
  for (const auto &Block : Function.Blocks)
    for (const auto &Boundary : Block.InstructionBoundaries)
      Result.insert(Boundary.Address);
  return Result;
}

TEST(ConstantBranch, ArithmeticTargetSkipsUnreachableOpcode) {
  // mov eax,0x1000; add eax,11; jmp eax/rax; db E9; ret.
  for (auto Architecture : {Arch::X86, Arch::X64})
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
      const auto Function = build({0xb8, 0x00, 0x10, 0x00, 0x00, 0x83, 0xc0,
                                   0x0b, 0xff, 0xe0, 0xe9, 0xc3},
                                  Architecture, Format);
      EXPECT_EQ(addresses(Function),
                (std::set<va_t>{0x1000, 0x1005, 0x1008, 0x100b}));
      ASSERT_EQ(Function.Blocks.size(), 2u);
      EXPECT_EQ(Function.Blocks.front().Succs.size(), 1u);
    }
}

TEST(ConstantBranch, UnknownLiveInAndPartialRegisterStayUnknown) {
  const auto Unknown = build({0x48, 0x83, 0xc0, 0x07, 0xff, 0xe0, 0xe9, 0xc3});
  EXPECT_FALSE(addresses(Unknown).count(0x1007));
  // Writing AX cannot manufacture the remaining 48 bits of RAX.
  const auto Partial =
      build({0x66, 0xb8, 0x08, 0x10, 0xff, 0xe0, 0xe9, 0x90, 0xc3});
  EXPECT_FALSE(addresses(Partial).count(0x1008));
}

TEST(ConstantBranch, CallsDoNotPreserveAnInventedRegisterValue) {
  const auto Function = build({0xb8, 0x0d, 0x10, 0x00, 0x00, 0xe8, 0xf6, 0x0f,
                               0x00, 0x00, 0xff, 0xe0, 0xe9, 0xc3});
  EXPECT_FALSE(addresses(Function).count(0x100d));
}

TEST(ConstantBranch, MemoryWithAnUnknownIndexCannotSupplyAConstantTarget) {
  // mov rax,[rdi*8+0x1018]; jmp rax. Slot zero happens to point at 0x1010,
  // but no input fact selects it and there is no finite switch domain.
  std::vector<uint8_t> Bytes = {0x48, 0x8b, 0x04, 0xfd, 0x18,
                                0x10, 0x00, 0x00, 0xff, 0xe0};
  Bytes.resize(0x18, 0x90);
  Bytes[0x10] = 0xc3;
  Bytes.insert(Bytes.end(), {0x10, 0x10, 0, 0, 0, 0, 0, 0});
  const auto Function = build(std::move(Bytes));
  EXPECT_FALSE(addresses(Function).count(0x1010));
  EXPECT_TRUE(Function.JumpTables.empty());
}

TEST(ConstantBranch, AResumableTrapCannotSupplyARegisterFact) {
  const auto Function = build(
      {0xb8, 0x0b, 0x10, 0x00, 0x00, 0xcc, 0x90, 0x90, 0xff, 0xe0, 0xe9, 0xc3});
  EXPECT_FALSE(addresses(Function).count(0x100b));
}

TEST(ConstantBranch, RepeatedBranchesKeepFlagsAndReachBothBodies) {
  // mov eax,0x1000; pushfq; add rax,15; popfq; jmp rax;
  // db E9; mov ecx,0x1000; add rcx,30; jmp rcx; db E9; nop; ret.
  const auto Function =
      build({0xb8, 0x00, 0x10, 0x00, 0x00, 0x9c, 0x48, 0x83, 0xc0, 0x0f, 0x9d,
             0xff, 0xe0, 0xe9, 0x90, 0xb9, 0x00, 0x10, 0x00, 0x00, 0x48, 0x83,
             0xc1, 0x1e, 0xff, 0xe1, 0xe9, 0x90, 0x90, 0x90, 0xc3});
  const auto Starts = addresses(Function);
  EXPECT_TRUE(Starts.count(0x100f));
  EXPECT_TRUE(Starts.count(0x101e));
  EXPECT_FALSE(Starts.count(0x100d));
  EXPECT_FALSE(Starts.count(0x101a));
  bool SavedFlags = false, RestoredFlags = false;
  for (const auto &Block : Function.Blocks)
    for (const auto &Boundary : Block.InstructionBoundaries) {
      SavedFlags |= Boundary.Address == 0x1005;
      RestoredFlags |= Boundary.Address == 0x100a;
    }
  EXPECT_TRUE(SavedFlags);
  EXPECT_TRUE(RestoredFlags);
}

TEST(ConstantBranch, ANewIncomingEdgeWithdrawsAnEarlierConstantProof) {
  const auto Function =
      build({0x85, 0xff, 0x74, 0x0d,       // test edi,edi; jz 0x1011
             0xb8, 0x1b, 0x10, 0x00, 0x00, // mov eax,0x101b
             0x83, 0xc0, 0x00, 0xff, 0xe0, // add eax,0; jmp rax
             0xe9, 0xc3, 0x90, 0xb9, 0x09,
             0x10, 0x00, 0x00, 0xff, 0xe1, // mov ecx,0x1009; jmp rcx
             0xe9, 0x90, 0x90, 0xc3});
  EXPECT_TRUE(addresses(Function).count(0x1009));
  EXPECT_FALSE(addresses(Function).count(0x101b));
  bool Found = false;
  for (const auto &Block : Function.Blocks)
    for (const auto &Boundary : Block.InstructionBoundaries)
      if (Boundary.Address == 0x100c) {
        Found = true;
        EXPECT_FALSE(Boundary.Immediate);
        EXPECT_TRUE(hasLowInstructionControlFlag(
            Boundary.ControlFlags, LowInstructionControlFlag::Indirect));
      }
  EXPECT_TRUE(Found);
  EXPECT_TRUE(Function.UnsafeIndirectBranchAddresses.count(0x100c));
}

TEST(ConstantBranch, AnOpcodeReachedByAnotherPathRemainsCode) {
  const auto Function =
      build({0x74, 0x0a, 0xb8, 0x0d, 0x10, 0x00, 0x00, 0x90, 0x90, 0x90, 0xff,
             0xe0, 0xe9, 0xc3, 0x90, 0x90, 0x90});
  EXPECT_TRUE(addresses(Function).count(0x100c));
  EXPECT_TRUE(addresses(Function).count(0x100d));
}

TEST(ConstantBranch, AArch64UsesTheSameExactScalarAuthority) {
  const auto Function = build({0x90, 0x01, 0x82, 0xd2, // mov x16,0x100c
                               0x00, 0x02, 0x1f, 0xd6, // br x16
                               0x00, 0x00, 0x20, 0xd4, // unreachable brk
                               0xc0, 0x03, 0x5f, 0xd6},
                              Arch::AArch64);
  EXPECT_EQ(addresses(Function), (std::set<va_t>{0x1000, 0x1004, 0x100c}));
}

} // namespace
