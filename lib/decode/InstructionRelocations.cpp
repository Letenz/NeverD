//===- InstructionRelocations.cpp - Relocations inside an instruction -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/decode/InstructionRelocations.h"

#include "neverd/loader/BinaryImage.h"

#include <cstring>

namespace neverd {

InstructionRelocations instructionRelocations(const BinaryImage &Img,
                                              const DecodedInsn &DI) {
  const va_t Cur = DI.Addr;
  const int Sz = DI.Size;
  const va_t Next = Cur + static_cast<va_t>(Sz);
  InstructionRelocations Result;
  std::vector<RelocatedAddressOperand> &RelocatedOperands = Result.Addresses;
  auto selectRelocatedOperand =
      [&](const std::map<va_t, RelocatedAddressField> &Occurrences,
          ConstantAddressProvenance Provenance) {
        auto It = Occurrences.lower_bound(Cur);
        for (; It != Occurrences.end() && It->first < Next; ++It) {
          va_t TargetVA = It->second.TargetVA;
          if (It->second.PCRelativeFromInstructionEnd) {
            if (It->second.Width == 0 || It->second.Width > 8)
              continue;
            const unsigned Bits = It->second.Width * 8;
            uint64_t Disp = It->second.EncodedValue;
            if (Bits < 64) {
              const uint64_t Mask = (uint64_t(1) << Bits) - 1;
              Disp &= Mask;
              if (Disp & (uint64_t(1) << (Bits - 1)))
                Disp |= ~Mask;
            }
            TargetVA = Next + Disp;
            if (Img.getPointerSize() == 4)
              TargetVA = static_cast<uint32_t>(TargetVA);
          }
          const bool OwnerMatches =
              It->second.Kind == RelocatedAddressFieldKind::I386ELFGOTOFF
                  ? Img.relocatedI386GOTOFFTargetBelongsToOwner(
                        TargetVA, It->second.TargetOwnerVA)
                  : Img.relocatedTargetBelongsToOwner(TargetVA,
                                                      It->second.TargetOwnerVA);
          if (!OwnerMatches)
            continue;
          RelocatedOperands.push_back(RelocatedAddressOperand{
              It->first, It->second.EncodedValue, TargetVA, It->second.Width,
              Provenance, It->second.TargetOwnerVA,
              It->second.PCRelativeFromInstructionEnd});
        }
      };
  selectRelocatedOperand(Img.DataAddressRelocOperands,
                         ConstantAddressProvenance::DataAddress);
  selectRelocatedOperand(Img.CodeAddressRelocOperands,
                         ConstantAddressProvenance::CodeAddress);
  std::vector<RelocatedScalarOperand> &RelocatedScalarOperands = Result.Scalars;
  if (Img.Arch == Arch::AArch64 && Img.isELF() && Img.IsRelocatable &&
      Img.ObjectRelocationWriteBytes && DI.Raw && Sz == 4) {
    const auto &Writes = *Img.ObjectRelocationWriteBytes;
    const auto Writer = Writes.lower_bound(Cur);
    if (Writer == Writes.end() || *Writer >= Next) {
      uint32_t Word = 0;
      for (unsigned Byte = 0; Byte != 4; ++Byte)
        Word |= uint32_t(DI.Raw->bytes[Byte]) << (Byte * 8);
      if ((Word & 0x1f800000u) == 0x12800000u)
        RelocatedScalarOperands.push_back(
            {Cur, Word, 4,
             RelocatedScalarOperand::Kind::AArch64ELFUnrelocatedWideMove});
    }
  }
  if (Img.Arch == Arch::X86 && Img.isELF() && Img.IsRelocatable &&
      Img.ObjectRelocationWriteBytes && DI.Raw && DI.Raw->detail) {
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
  if (Img.Arch == Arch::X86 && Img.isELF() && Img.getPointerSize() == 4) {
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
  return Result;
}

} // namespace neverd
