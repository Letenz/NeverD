//===- LinkerThunk.cpp - Thunks a linker writes that only jump on ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/LinkerThunk.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/DirectBranch.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/BranchEncoding.h"
#include "neverd/support/InstructionFields.h"

#include "llvm/Support/MathExtras.h"

namespace neverd {
namespace {

using namespace branch;

uint64_t wrapToImage(const BinaryImage &Img, uint64_t Address) {
  return Img.is64Bit() ? Address : static_cast<uint32_t>(Address);
}

/// The word at \p Address, when the image holds it.
std::optional<uint32_t> wordAt(const BinaryImage &Img, uint64_t Address) {
  const uint8_t *Bytes = Img.readVA(Address, sizeof(uint32_t));
  if (!Bytes)
    return std::nullopt;
  return readLE<uint32_t>(Bytes);
}

/// The halfword at \p Address, when the image holds it.
std::optional<uint16_t> halfwordAt(const BinaryImage &Img, uint64_t Address) {
  const uint8_t *Bytes = Img.readVA(Address, sizeof(uint16_t));
  if (!Bytes)
    return std::nullopt;
  return readLE<uint16_t>(Bytes);
}

/// The unconditional direct jump at \p Address, in \p Mode.  A 32-bit
/// image's addresses wrap at 32 bits; an A64 program counter is 64 bits wide
/// whatever the image's pointers are.
std::optional<LinkerThunk> directJump(const BinaryImage &Img, uint64_t Address,
                                      InstructionMode Mode) {
  const size_t Length = getDirectBranchLength(Img.Arch);
  const uint8_t *Insn = Img.readVA(Address, Length);
  if (!Insn)
    return std::nullopt;
  DirectBranchForms Forms;
  Forms.Jumps = true;
  const std::optional<DirectBranch> Decoded =
      decodeDirectBranch(Img.Arch, Mode, Insn, Length, Address, Forms);
  if (!Decoded)
    return std::nullopt;
  const uint64_t Target = Decoded->wrappingTarget();
  LinkerThunk Thunk;
  Thunk.Target = Img.Arch == Arch::AArch64 ? Target : wrapToImage(Img, Target);
  Thunk.Thumb = Decoded->TargetIsThumb;
  Thunk.Size = static_cast<uint8_t>(Length);
  Thunk.DirectJump = true;
  return Thunk;
}

/// A thunk of \p Size bytes that enters the 32-bit ARM code address
/// \p Address in the state its low bit says.
LinkerThunk interworkingThunk(const BinaryImage &Img, uint64_t Address,
                              uint8_t Size) {
  LinkerThunk Thunk;
  Thunk.Target = wrapToImage(Img, Address & ~uint64_t(ThumbStateBit));
  Thunk.Thumb = (Address & ThumbStateBit) != 0;
  Thunk.Size = Size;
  return Thunk;
}

/// The immediate an ARM-state MOVW (or, when \p Top, MOVT) writes to ip.
std::optional<BitString> armMoveToIP(uint32_t Word, bool Top) {
  if (!(Top ? ArmMoveTopToIP : ArmMoveWideToIP).matches(Word))
    return std::nullopt;
  return BitString().append(ArmMoveImm4, Word).append(ArmMoveImm12, Word);
}

/// The immediate a Thumb-2 MOVW (or, when \p Top, MOVT) at \p Address
/// writes to ip.
std::optional<BitString> thumbMoveToIP(const BinaryImage &Img, uint64_t Address,
                                       bool Top) {
  const std::optional<uint16_t> First = halfwordAt(Img, Address);
  const std::optional<uint16_t> Second =
      halfwordAt(Img, Address + ThumbHalfwordBytes);
  if (!First || !Second ||
      !(Top ? ThumbMoveTopToIPHigh : ThumbMoveWideToIPHigh).matches(*First) ||
      !ThumbMoveToIPLow.matches(*Second))
    return std::nullopt;
  return BitString()
      .append(ThumbMoveImm4, *First)
      .append(ThumbMoveI, *First)
      .append(ThumbMoveImm3, *Second)
      .append(ThumbMoveImm8, *Second);
}

/// An ARM-state long branch at \p Address: `ldr pc, [pc, #-4]` and the
/// address after it (ARMv5 and GNU long branches), or lld's
/// `movw ip; movt ip; [add ip, ip, pc;] bx ip`.
std::optional<LinkerThunk> armLongBranch(const BinaryImage &Img,
                                         uint64_t Address) {
  const uint64_t SecondAddress = Address + ArmInstructionBytes;
  const std::optional<uint32_t> First = wordAt(Img, Address),
                                Second = wordAt(Img, SecondAddress);
  if (!First || !Second)
    return std::nullopt;
  if (ArmLoadPCFromNextWord.matches(*First))
    return interworkingThunk(Img, *Second, 2 * ArmInstructionBytes);
  const std::optional<BitString> Low = armMoveToIP(*First, /*Top=*/false);
  const std::optional<BitString> High =
      Low ? armMoveToIP(*Second, /*Top=*/true) : std::nullopt;
  const uint64_t ThirdAddress = SecondAddress + ArmInstructionBytes;
  const std::optional<uint32_t> Third =
      High ? wordAt(Img, ThirdAddress) : std::nullopt;
  if (!Third)
    return std::nullopt;
  const uint64_t Value = BitString(*High).append(*Low).zeroExtended();
  if (ArmBranchExchangeIP.matches(*Third))
    return interworkingThunk(Img, Value, 3 * ArmInstructionBytes);
  // `add ip, ip, pc` reads pc eight bytes past itself.
  if (ArmAddIPIPPC.matches(*Third)) {
    const std::optional<uint32_t> Fourth =
        wordAt(Img, ThirdAddress + ArmInstructionBytes);
    if (Fourth && ArmBranchExchangeIP.matches(*Fourth))
      return interworkingThunk(Img, Value + ThirdAddress + ArmPCOffset,
                               4 * ArmInstructionBytes);
  }
  return std::nullopt;
}

/// A Thumb long branch at \p Address: lld's
/// `movw ip; movt ip; [add ip, pc;] bx ip`.
std::optional<LinkerThunk> thumbLongBranch(const BinaryImage &Img,
                                           uint64_t Address) {
  const uint64_t SecondAddress = Address + ThumbWideInstructionBytes;
  const std::optional<BitString> Low =
      thumbMoveToIP(Img, Address, /*Top=*/false);
  const std::optional<BitString> High =
      Low ? thumbMoveToIP(Img, SecondAddress, /*Top=*/true) : std::nullopt;
  if (!High)
    return std::nullopt;
  const uint64_t Value = BitString(*High).append(*Low).zeroExtended();
  const uint64_t ThirdAddress = SecondAddress + ThumbWideInstructionBytes;
  const std::optional<uint16_t> Third = halfwordAt(Img, ThirdAddress);
  if (!Third)
    return std::nullopt;
  if (ThumbBranchExchangeIP.matches(*Third))
    return interworkingThunk(
        Img, Value, 2 * ThumbWideInstructionBytes + ThumbHalfwordBytes);
  // `add ip, pc` reads pc four bytes past itself.
  if (ThumbAddIPPC.matches(*Third)) {
    const std::optional<uint16_t> Fourth =
        halfwordAt(Img, ThirdAddress + ThumbHalfwordBytes);
    if (Fourth && ThumbBranchExchangeIP.matches(*Fourth))
      return interworkingThunk(Img, Value + ThirdAddress + ThumbPCOffset,
                               2 * ThumbWideInstructionBytes +
                                   2 * ThumbHalfwordBytes);
  }
  return std::nullopt;
}

/// An A64 long branch at \p Address: `adrp x16; add x16, x16, #lo12;
/// br x16`, or `ldr x16, #8; br x16` and the address after them.
std::optional<LinkerThunk> aarch64LongBranch(const BinaryImage &Img,
                                             uint64_t Address) {
  const std::optional<uint32_t> First = wordAt(Img, Address);
  if (!First)
    return std::nullopt;
  const uint64_t SecondAddress = Address + A64InstructionBytes;
  const uint64_t ThirdAddress = SecondAddress + A64InstructionBytes;
  const std::optional<uint32_t> Second = wordAt(Img, SecondAddress),
                                Third = wordAt(Img, ThirdAddress);
  if (!Second || !Third)
    return std::nullopt;
  LinkerThunk Thunk;
  if (A64PageAddressX16.matches(*First) && A64AddX16X16.matches(*Second) &&
      A64BranchRegisterX16.matches(*Third)) {
    const int64_t Pages = BitString()
                              .append(A64PageImmHi, *First)
                              .append(A64PageImmLo, *First)
                              .signExtended();
    const uint64_t Page = llvm::alignDown(Address, A64PageBytes) +
                          static_cast<uint64_t>(Pages * A64PageBytes);
    Thunk.Target = Page + A64AddImm12.extract(*Second);
    Thunk.Size = 3 * A64InstructionBytes;
    return Thunk;
  }
  if (A64LoadX16FromThunkEnd.matches(*First) &&
      A64BranchRegisterX16.matches(*Second)) {
    const uint64_t Literal =
        Address + A64LoadLiteralOffset.extract(*First) * A64InstructionBytes;
    if (const uint8_t *Bytes = Img.readVA(Literal, sizeof(uint64_t))) {
      Thunk.Target = readLE<uint64_t>(Bytes);
      Thunk.Size = static_cast<uint8_t>(Literal + sizeof(uint64_t) - Address);
      return Thunk;
    }
  }
  return std::nullopt;
}

} // namespace

std::optional<LinkerThunk> linkerLongBranchThunkAt(const BinaryImage &Img,
                                                   uint64_t Address,
                                                   InstructionMode Mode) {
  switch (Img.Arch) {
  case Arch::AArch64:
    return aarch64LongBranch(Img, Address);
  case Arch::ARM:
    return Mode == InstructionMode::Thumb ? thumbLongBranch(Img, Address)
                                          : armLongBranch(Img, Address);
  default:
    return std::nullopt;
  }
}

std::optional<LinkerThunk>
linkerThunkAt(const BinaryImage &Img, uint64_t Address, InstructionMode Mode) {
  switch (Img.Arch) {
  case Arch::X86:
  case Arch::X64:
  case Arch::AArch64:
    Mode = InstructionMode::Default;
    break;
  case Arch::ARM:
    if (Mode != InstructionMode::Thumb)
      Mode = InstructionMode::ARM;
    break;
  default:
    return std::nullopt;
  }
  if (std::optional<LinkerThunk> Jump = directJump(Img, Address, Mode))
    return Jump;
  return linkerLongBranchThunkAt(Img, Address, Mode);
}

} // namespace neverd
