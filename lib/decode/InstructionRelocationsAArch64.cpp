//===- InstructionRelocationsAArch64.cpp - AArch64 relocation fields -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "InstructionRelocationsDetail.h"

#include "neverd/loader/BinaryImage.h"

namespace neverd {
namespace instruction_relocations_detail {

void addAArch64Fields(const BinaryImage &Img, const DecodedInsn &DI,
                      InstructionRelocations &Result) {
  const va_t Cur = DI.Addr;
  const va_t Next = Cur + static_cast<va_t>(DI.Size);
  // An unlinked object's MOVZ/MOVN that no relocation writes keeps its
  // encoded number.
  if (Img.isELF() && Img.IsRelocatable && Img.ObjectRelocationWriteBytes &&
      DI.Raw && DI.Size == 4) {
    const auto &Writes = *Img.ObjectRelocationWriteBytes;
    const auto Writer = Writes.lower_bound(Cur);
    if (Writer == Writes.end() || *Writer >= Next) {
      uint32_t Word = 0;
      for (unsigned Byte = 0; Byte != 4; ++Byte)
        Word |= uint32_t(DI.Raw->bytes[Byte]) << (Byte * 8);
      if ((Word & 0x1f800000u) == 0x12800000u)
        Result.Scalars.push_back(
            {Cur, Word, 4,
             RelocatedScalarOperand::Kind::AArch64ELFUnrelocatedWideMove});
    }
  }
}

} // namespace instruction_relocations_detail
} // namespace neverd
