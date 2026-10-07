//===- InstructionRelocationsX86.cpp - x86 relocation fields -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "InstructionRelocationsDetail.h"

#include "neverd/Limits.h"
#include "neverd/loader/BinaryImage.h"

#include <cstring>

namespace neverd {
namespace instruction_relocations_detail {
namespace {

/// In an image that runs at its link address, the displacement of a memory
/// operand that lands in a mapped segment is that address, as a relocation
/// would have said: `jmp *table(,%rdi,8)` and `movzbl array(%rcx), %eax`
/// index an object of the image.  A field offset added to a pointer
/// (`mov 8(,%rcx,8), %rax` on a packed pointer) lands in no segment, the
/// image being mapped above it, and stays a number.  RIP-relative operands
/// and a plain `[disp]` are addresses for the lifter already.
void addFixedImageDisplacement(const BinaryImage &Img, const DecodedInsn &DI,
                               InstructionRelocations &Result) {
  if (!Img.LoadsAtLinkAddress || !DI.Raw || !DI.Raw->detail)
    return;
  const cs_x86 &X86 = DI.Raw->detail->x86;
  const uint8_t Width = X86.encoding.disp_size;
  const uint8_t Offset = X86.encoding.disp_offset;
  if (Width == 0 || Offset == 0 || Offset + Width > DI.Size)
    return;
  const cs_x86_op *Memory = nullptr;
  for (uint8_t I = 0; I < X86.op_count; ++I)
    if (X86.operands[I].type == X86_OP_MEM) {
      Memory = &X86.operands[I];
      break;
    }
  if (!Memory || Memory->mem.base == X86_REG_RIP ||
      Memory->mem.base == X86_REG_EIP ||
      (Memory->mem.base == X86_REG_INVALID &&
       Memory->mem.index == X86_REG_INVALID) ||
      Memory->mem.segment == X86_REG_FS || Memory->mem.segment == X86_REG_GS)
    return;
  const unsigned AddressBytes =
      X86.addr_size ? X86.addr_size : Img.getPointerSize();
  uint64_t Target = static_cast<uint64_t>(Memory->mem.disp);
  if (AddressBytes < 8)
    Target &= (uint64_t(1) << (AddressBytes * 8)) - 1;
  const Segment *Seg = Img.getSegmentFor(Target);
  if (!Seg || Target < limits::kMinGlobalDataAddr)
    return;
  // A relocation the loader recorded for the field already owns it.
  const va_t FieldVA = DI.Addr + Offset;
  for (const RelocatedAddressOperand &Existing : Result.Addresses)
    if (Existing.FieldVA == FieldVA)
      return;
  uint64_t Encoded = 0;
  std::memcpy(&Encoded, DI.Raw->bytes + Offset, Width);
  Result.Addresses.push_back(RelocatedAddressOperand{
      FieldVA, Encoded, Target, Width,
      Seg->isExecutable() ? ConstantAddressProvenance::CodeAddress
                          : ConstantAddressProvenance::DataAddress,
      InvalidVA, /*PCRelativeFromInstructionEnd=*/false});
}

/// i386 ELF fields the loader recorded: an unlinked object's immediate no
/// relocation writes, and the GOT-pointer fields of position-independent
/// code.
void addI386ELFFields(const BinaryImage &Img, const DecodedInsn &DI,
                      InstructionRelocations &Result) {
  const va_t Cur = DI.Addr;
  const int Sz = DI.Size;
  const va_t Next = Cur + static_cast<va_t>(Sz);
  std::vector<RelocatedScalarOperand> &RelocatedScalarOperands = Result.Scalars;
  if (Img.IsRelocatable && Img.ObjectRelocationWriteBytes && DI.Raw &&
      DI.Raw->detail) {
    const auto &Encoding = DI.Raw->detail->x86.encoding;
    const uint8_t Width = Encoding.imm_size;
    if (Width > 0 && Width <= 4 && Encoding.imm_offset > 0 &&
        Encoding.imm_offset + Width <= Sz) {
      const va_t FieldVA = Cur + Encoding.imm_offset;
      const auto &Writes = *Img.ObjectRelocationWriteBytes;
      const auto Writer = Writes.lower_bound(FieldVA);
      if (Writer == Writes.end() || *Writer >= FieldVA + Width) {
        uint64_t Encoded = 0;
        for (uint8_t Byte = 0; Byte < Width; ++Byte)
          Encoded |= uint64_t(DI.Raw->bytes[Encoding.imm_offset + Byte])
                     << (Byte * 8);
        RelocatedScalarOperands.push_back(
            {FieldVA, Encoded, Width,
             RelocatedScalarOperand::Kind::I386ELFUnrelocatedImmediate});
      }
    }
  }
  if (Img.getPointerSize() != 4)
    return;
  auto Field = Img.I386GOTPCFields.lower_bound(Cur);
  for (; Field != Img.I386GOTPCFields.end() && Field->first < Next; ++Field)
    RelocatedScalarOperands.push_back(
        {Field->first, Field->second.EncodedValue, 4,
         RelocatedScalarOperand::Kind::I386ELFGOTPC});
  auto Ambiguous = Img.AmbiguousI386GOTPCFields.lower_bound(Cur);
  for (; Ambiguous != Img.AmbiguousI386GOTPCFields.end() && *Ambiguous < Next;
       ++Ambiguous) {
    const uint8_t *EncodedBytes = Img.readVA(*Ambiguous, 4);
    if (!EncodedBytes)
      continue;
    uint32_t Encoded = 0;
    std::memcpy(&Encoded, EncodedBytes, sizeof(Encoded));
    RelocatedScalarOperands.push_back(
        {*Ambiguous, Encoded, 4, RelocatedScalarOperand::Kind::I386ELFGOTPC});
  }
  auto AmbiguousGOTOFF = Img.AmbiguousI386GOTOFFFields.lower_bound(Cur);
  for (; AmbiguousGOTOFF != Img.AmbiguousI386GOTOFFFields.end() &&
         *AmbiguousGOTOFF < Next;
       ++AmbiguousGOTOFF) {
    const uint8_t *EncodedBytes = Img.readVA(*AmbiguousGOTOFF, 4);
    if (!EncodedBytes)
      continue;
    uint32_t Encoded = 0;
    std::memcpy(&Encoded, EncodedBytes, sizeof(Encoded));
    RelocatedScalarOperands.push_back(
        {*AmbiguousGOTOFF, Encoded, 4,
         RelocatedScalarOperand::Kind::I386ELFAmbiguousGOTOFF});
  }
}

} // namespace

void addX86Fields(const BinaryImage &Img, const DecodedInsn &DI,
                  InstructionRelocations &Result) {
  if (Img.Arch == Arch::X86 && Img.isELF())
    addI386ELFFields(Img, DI, Result);
  addFixedImageDisplacement(Img, DI, Result);
}

} // namespace instruction_relocations_detail
} // namespace neverd
