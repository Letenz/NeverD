//===- DirectBranchDecodeTests.cpp - decodeDirectBranch tests -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/DirectBranch.h"

#include "llvm/ADT/ArrayRef.h"

using namespace neverd;

namespace {

/// Decode \p Bytes at \p VA, accepting \p Forms.
std::optional<DirectBranch> decodeAt(Arch A, InstructionMode Mode,
                                     llvm::ArrayRef<uint8_t> Bytes, va_t VA,
                                     DirectBranchForms Forms) {
  return decodeDirectBranch(A, Mode, Bytes.data(), Bytes.size(), VA, Forms);
}

DirectBranchForms allForms() {
  DirectBranchForms Forms;
  Forms.Jumps = true;
  Forms.Calls = true;
  Forms.ExchangingCalls = true;
  Forms.Conditional = true;
  return Forms;
}

TEST(DirectBranchDecode, DecodesEachKindOnlyWhenAsked) {
  DirectBranchForms JumpsOnly;
  JumpsOnly.Jumps = true;
  DirectBranchForms CallsOnly;
  CallsOnly.Calls = true;

  // x86 `call rel32` and `jmp rel32` count from the next instruction.
  const uint8_t X86Call[] = {0xE8, 0xFB, 0x00, 0x00, 0x00};
  const uint8_t X86Jump[] = {0xE9, 0xFB, 0x00, 0x00, 0x00};
  const auto Call =
      decodeAt(Arch::X64, InstructionMode::Default, X86Call, 0x1000, CallsOnly);
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->Kind, DirectBranchKind::Call);
  EXPECT_EQ(Call->Length, 5u);
  EXPECT_EQ(Call->target(), va_t(0x1100));
  EXPECT_FALSE(decodeAt(Arch::X64, InstructionMode::Default, X86Call, 0x1000,
                        JumpsOnly));
  const auto Jump =
      decodeAt(Arch::X86, InstructionMode::Default, X86Jump, 0x1000, JumpsOnly);
  ASSERT_TRUE(Jump);
  EXPECT_EQ(Jump->Kind, DirectBranchKind::Jump);
  EXPECT_FALSE(decodeAt(Arch::X86, InstructionMode::Default, X86Jump, 0x1000,
                        CallsOnly));

  // A64 `b` and `bl` count from themselves, in instructions.
  const uint8_t A64B[] = {0x40, 0x00, 0x00, 0x14};
  const uint8_t A64BL[] = {0x40, 0x00, 0x00, 0x94};
  const auto B = decodeAt(Arch::AArch64, InstructionMode::Default, A64B, 0x1000,
                          JumpsOnly);
  ASSERT_TRUE(B);
  EXPECT_EQ(B->target(), va_t(0x1100));
  EXPECT_FALSE(decodeAt(Arch::AArch64, InstructionMode::Default, A64B, 0x1000,
                        CallsOnly));
  const auto BL = decodeAt(Arch::AArch64, InstructionMode::Default, A64BL,
                           0x1000, CallsOnly);
  ASSERT_TRUE(BL);
  EXPECT_EQ(BL->Kind, DirectBranchKind::Call);
}

TEST(DirectBranchDecode, TellsA32ConditionAndStateApart) {
  DirectBranchForms Unconditional = allForms();
  Unconditional.Conditional = false;

  // `0b00003e` is `bleq` 0x100 bytes past the pipeline offset.
  const uint8_t CallIfEqual[] = {0x3E, 0x00, 0x00, 0x0B};
  const auto Conditional = decodeAt(Arch::ARM, InstructionMode::ARM,
                                    CallIfEqual, 0x1000, allForms());
  ASSERT_TRUE(Conditional);
  EXPECT_EQ(Conditional->Kind, DirectBranchKind::Call);
  EXPECT_TRUE(Conditional->Conditional);
  EXPECT_EQ(Conditional->target(), va_t(0x1100));
  EXPECT_FALSE(decodeAt(Arch::ARM, InstructionMode::ARM, CallIfEqual, 0x1000,
                        Unconditional));

  // `fa00003e` is `blx` to Thumb code; with the H bit, `fb00003e` reaches the
  // halfword after it.
  const uint8_t Exchange[] = {0x3E, 0x00, 0x00, 0xFA};
  const uint8_t ExchangeHalfword[] = {0x3E, 0x00, 0x00, 0xFB};
  const auto ToThumb =
      decodeAt(Arch::ARM, InstructionMode::ARM, Exchange, 0x1000, allForms());
  ASSERT_TRUE(ToThumb);
  EXPECT_EQ(ToThumb->Kind, DirectBranchKind::ExchangingCall);
  EXPECT_FALSE(ToThumb->Conditional);
  EXPECT_TRUE(ToThumb->TargetIsThumb);
  EXPECT_EQ(ToThumb->target(), va_t(0x1100));
  const auto ToThumbHalfword = decodeAt(Arch::ARM, InstructionMode::ARM,
                                        ExchangeHalfword, 0x1000, allForms());
  ASSERT_TRUE(ToThumbHalfword);
  EXPECT_EQ(ToThumbHalfword->target(), va_t(0x1102));

  // decodeDirectBranchTarget takes Thumb's `blx` but not A32's.
  size_t Length = 0;
  EXPECT_FALSE(decodeDirectBranchTarget(Arch::ARM, InstructionMode::ARM,
                                        Exchange, sizeof(Exchange), 0x1000,
                                        Length));
}

TEST(DirectBranchDecode, RejectsThumbExchangeWithTheHalfwordBitSet) {
  // `f000 e87e` is `blx`, and the same with H set is UNDEFINED, whatever the
  // caller accepts.
  const uint8_t Exchange[] = {0x00, 0xF0, 0x7E, 0xE8};
  const uint8_t Undefined[] = {0x00, 0xF0, 0x7F, 0xE8};
  const auto ToARM =
      decodeAt(Arch::ARM, InstructionMode::Thumb, Exchange, 0x1002, allForms());
  ASSERT_TRUE(ToARM);
  EXPECT_EQ(ToARM->Kind, DirectBranchKind::ExchangingCall);
  EXPECT_FALSE(ToARM->TargetIsThumb);
  EXPECT_EQ(ToARM->target(), va_t(0x1100));
  EXPECT_FALSE(decodeAt(Arch::ARM, InstructionMode::Thumb, Undefined, 0x1002,
                        allForms()));
}

TEST(DirectBranchDecode, TargetDoesNotWrapAroundTheAddressSpace) {
  const uint8_t Backward[] = {0xE8, 0x00, 0xFF, 0xFF, 0xFF};
  const auto BelowZero =
      decodeAt(Arch::X64, InstructionMode::Default, Backward, 0x10, allForms());
  ASSERT_TRUE(BelowZero);
  EXPECT_FALSE(BelowZero->target());
  EXPECT_EQ(BelowZero->wrappingTarget(), va_t(0x10 + 5 - 0x100));

  const uint8_t Forward[] = {0xE8, 0x00, 0x01, 0x00, 0x00};
  const auto PastTheTop = decodeAt(Arch::X64, InstructionMode::Default, Forward,
                                   InvalidVA - 0x80, allForms());
  ASSERT_TRUE(PastTheTop);
  EXPECT_FALSE(PastTheTop->target());

  const uint8_t A64B[] = {0x40, 0x00, 0x00, 0x14};
  const auto NearTheTop = decodeAt(Arch::AArch64, InstructionMode::Default,
                                   A64B, InvalidVA - 0x7F, allForms());
  ASSERT_TRUE(NearTheTop);
  EXPECT_FALSE(NearTheTop->target());
}

} // namespace
