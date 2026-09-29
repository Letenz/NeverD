//===- DirectBranch.cpp - Direct branch decoding and scanning -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/DirectBranch.h"

#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/ISAEncoding.h"
#include "neverd/support/InstructionFields.h"

#include "llvm/Support/MathExtras.h"

namespace neverd {
namespace {

#define NEVERD_BRANCH_FORM(Name, Mask, Match)                                  \
  [[maybe_unused]] constexpr InstructionForm Name{Mask, Match};
#define NEVERD_BRANCH_FIELD(Name, Low, Width)                                  \
  [[maybe_unused]] constexpr BitField Name{Low, Width};
#define NEVERD_BRANCH_VALUE(Name, Value)                                       \
  [[maybe_unused]] constexpr uint32_t Name = Value;
#include "neverd/support/BranchEncoding.def"

static_assert(x86::kCallRel32Len == x86::kJmpRel32Len,
              "a rel32 call and jump are read alike");
static_assert(ArmInstructionBytes == ThumbWideInstructionBytes,
              "every ARM direct branch is a word long");

using Kind = DirectBranchKind;

/// Whether \p Forms takes a branch of kind \p K.
bool accepts(DirectBranchForms Forms, Kind K, bool Conditional) {
  if (Conditional && !Forms.Conditional)
    return false;
  switch (K) {
  case Kind::Jump:
    return Forms.Jumps;
  case Kind::Call:
    return Forms.Calls;
  case Kind::ExchangingCall:
    return Forms.ExchangingCalls;
  }
  return false;
}

/// An x86 `call rel32` or `jmp rel32`, whose displacement counts from the
/// next instruction.
std::optional<DirectBranch> decodeX86(const uint8_t *Code, size_t Available,
                                      va_t VA, DirectBranchForms Forms) {
  if (Available < x86::kCallRel32Len)
    return std::nullopt;
  DirectBranch Branch;
  if (Code[0] == x86::kCallRel32)
    Branch.Kind = Kind::Call;
  else if (Code[0] == x86::kJmpRel32)
    Branch.Kind = Kind::Jump;
  else
    return std::nullopt;
  if (!accepts(Forms, Branch.Kind, /*Conditional=*/false))
    return std::nullopt;
  Branch.Address = VA;
  Branch.Length = x86::kCallRel32Len;
  Branch.Base = VA + x86::kCallRel32Len;
  Branch.Displacement = readLE<int32_t>(Code + x86::kRel32DispOffset);
  return Branch;
}

/// An A64 `b` or `bl`, whose offset in instructions counts from itself.
std::optional<DirectBranch> decodeA64(const uint8_t *Code, size_t Available,
                                      va_t VA, DirectBranchForms Forms) {
  if (Available < A64InstructionBytes)
    return std::nullopt;
  const uint32_t Word = readLE<uint32_t>(Code);
  if (!A64BranchOrLink.matches(Word))
    return std::nullopt;
  DirectBranch Branch;
  Branch.Kind = A64Branch.matches(Word) ? Kind::Jump : Kind::Call;
  if (!accepts(Forms, Branch.Kind, /*Conditional=*/false))
    return std::nullopt;
  Branch.Address = VA;
  Branch.Length = A64InstructionBytes;
  Branch.Base = VA;
  Branch.Displacement =
      BitString().append(A64BranchOffset, Word).signExtended() *
      A64InstructionBytes;
  return Branch;
}

/// An A32 `b` or `bl` under any condition, or `blx` (immediate), whose offset
/// in words counts from the program counter eight bytes ahead; `blx` adds
/// its halfword bit.
std::optional<DirectBranch> decodeA32(const uint8_t *Code, size_t Available,
                                      va_t VA, DirectBranchForms Forms) {
  if (Available < ArmInstructionBytes)
    return std::nullopt;
  const uint32_t Word = readLE<uint32_t>(Code);
  if (!ArmBranch.matches(Word))
    return std::nullopt;
  const uint32_t Condition = ArmCondition.extract(Word);
  // BL's link bit, and BLX's halfword bit.
  const bool LinkOrHalfword = ArmLinkOrHalfword.extract(Word);
  const bool Exchange = Condition == ArmConditionUnconditional;
  DirectBranch Branch;
  Branch.Kind = Exchange         ? Kind::ExchangingCall
                : LinkOrHalfword ? Kind::Call
                                 : Kind::Jump;
  Branch.Conditional = !Exchange && Condition != ArmConditionAlways;
  if (!accepts(Forms, Branch.Kind, Branch.Conditional))
    return std::nullopt;
  Branch.Address = VA;
  Branch.Length = ArmInstructionBytes;
  Branch.Base = VA + ArmPCOffset;
  Branch.Displacement =
      BitString().append(ArmBranchOffset, Word).signExtended() *
      ArmInstructionBytes;
  if (Exchange && LinkOrHalfword)
    Branch.Displacement += ThumbHalfwordBytes;
  Branch.TargetIsThumb = Exchange;
  return Branch;
}

/// A T32 `b.w` (T4), `bl` or `blx` (immediate), whose offset in halfwords
/// counts from the program counter four bytes ahead -- rounded down to a word
/// for `blx`, which targets ARM state, where instructions are words.
///
/// The three share one encoding split across two halfwords: the first carries
/// the sign and the high immediate bits, the second the J1/J2 pair that --
/// exclusive-ored back against the sign -- restores the two immediate bits
/// above them.  Conditional `b.w` (T3) shares the leading halfword but names a
/// branch within the function; it is not decoded.
std::optional<DirectBranch> decodeT32(const uint8_t *Code, size_t Available,
                                      va_t VA, DirectBranchForms Forms) {
  if (Available < ThumbWideInstructionBytes)
    return std::nullopt;
  const uint16_t First = readLE<uint16_t>(Code);
  const uint16_t Second = readLE<uint16_t>(Code + ThumbHalfwordBytes);
  if (!ThumbWideBranchHigh.matches(First))
    return std::nullopt;
  DirectBranch Branch;
  if (ThumbJumpLow.matches(Second))
    Branch.Kind = Kind::Jump;
  else if (ThumbLinkLow.matches(Second))
    Branch.Kind = Kind::Call;
  else if (ThumbLinkExchangeLow.matches(Second))
    Branch.Kind = Kind::ExchangingCall;
  else
    return std::nullopt;
  if (!accepts(Forms, Branch.Kind, /*Conditional=*/false))
    return std::nullopt;
  const bool Exchange = Branch.Kind == Kind::ExchangingCall;
  // The offset's I1 and I2 are J1 and J2 against its sign.
  const uint32_t Sign = ThumbBranchSign.extract(First);
  Branch.Address = VA;
  Branch.Length = ThumbWideInstructionBytes;
  Branch.Base = VA + ThumbPCOffset;
  if (Exchange)
    Branch.Base = llvm::alignDown(Branch.Base, ArmInstructionBytes);
  Branch.Displacement =
      BitString()
          .append(ThumbBranchSign, First)
          .append(!(ThumbBranchJ1.extract(Second) ^ Sign), ThumbBranchJ1.Width)
          .append(!(ThumbBranchJ2.extract(Second) ^ Sign), ThumbBranchJ2.Width)
          .append(ThumbBranchHigh, First)
          .append(ThumbBranchLow, Second)
          .signExtended() *
      ThumbHalfwordBytes;
  Branch.TargetIsThumb = !Exchange;
  return Branch;
}

} // namespace

std::optional<va_t> DirectBranch::target() const {
  // The program counter the displacement counts from already wrapped.
  if (Base < Address)
    return std::nullopt;
  const va_t Magnitude = Displacement < 0 ? 0 - static_cast<va_t>(Displacement)
                                          : static_cast<va_t>(Displacement);
  if (Displacement < 0 ? Magnitude > Base : Magnitude > InvalidVA - Base)
    return std::nullopt;
  return wrappingTarget();
}

std::optional<DirectBranch> decodeDirectBranch(Arch A, InstructionMode Mode,
                                               const uint8_t *Code,
                                               size_t Available, va_t VA,
                                               DirectBranchForms Forms) {
  if (!isSingleInstructionMode(Mode))
    return std::nullopt;
  switch (A) {
  case Arch::X64:
  case Arch::X86:
    return decodeX86(Code, Available, VA, Forms);
  case Arch::AArch64:
    return decodeA64(Code, Available, VA, Forms);
  case Arch::ARM:
    return Mode == InstructionMode::Thumb
               ? decodeT32(Code, Available, VA, Forms)
               : decodeA32(Code, Available, VA, Forms);
  default:
    return std::nullopt;
  }
}

size_t getDirectBranchLength(Arch A) {
  switch (A) {
  case Arch::X64:
  case Arch::X86:
    return x86::kCallRel32Len;
  case Arch::AArch64:
    return A64InstructionBytes;
  case Arch::ARM:
    return ArmInstructionBytes;
  default:
    return 0;
  }
}

std::optional<va_t> decodeDirectBranchTarget(Arch A, InstructionMode Mode,
                                             const uint8_t *Code,
                                             size_t Available, va_t VA,
                                             size_t &Length) {
  DirectBranchForms Forms;
  Forms.Jumps = true;
  Forms.Calls = true;
  Forms.ExchangingCalls = A == Arch::ARM && Mode == InstructionMode::Thumb;
  const std::optional<DirectBranch> Branch =
      decodeDirectBranch(A, Mode, Code, Available, VA, Forms);
  if (!Branch)
    return std::nullopt;
  Length = Branch->Length;
  return Branch->target();
}

unsigned getBranchScanStride(Arch A, InstructionMode Mode) {
  if (A == Arch::X64 || A == Arch::X86)
    return X86InstructionAlignment;
  if (A == Arch::ARM && Mode == InstructionMode::Thumb)
    return ThumbHalfwordBytes;
  return A == Arch::AArch64 ? A64InstructionBytes : ArmInstructionBytes;
}

} // namespace neverd
