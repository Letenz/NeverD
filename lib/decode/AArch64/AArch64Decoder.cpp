//===- AArch64Decoder.cpp - AArch64 decoder support ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Native AArch64 decoding and scalar relocation handling for lifting.
///
//===----------------------------------------------------------------------===//

#include "neverd/decode/AArch64NativeDecode.h"
#include "neverd/decode/Decoder.h"
#include "neverd/lift/AArch64Lifter.h"

#include <cstdint>

namespace neverd {

int Decoder::decodeAArch64ForLift(const uint8_t *Bytes, size_t Len, va_t Addr,
                                  DecodedInsn &Out) {
  // AArch64 is fixed 4-byte width and 4-byte aligned; the common instruction
  // classes decode with a few mask+shift extractions straight into InsnBuf,
  // bypassing Capstone's DFA (the front-end throughput ceiling).  The native
  // decoder is a strict subset of Capstone's accept set and lifts identically
  // (locked by AArch64_NativeDecodeParityTests); anything it declines falls
  // back to the caller's full Capstone decode.  Detail must be on — the native
  // path fills operand detail the lifter reads — and InsnBuf->detail is
  // allocated whenever detail is on (see init()).
  if (AArch64 && Detail && Len >= 4 && InsnBuf && InsnBuf->detail) {
    uint32_t Word = static_cast<uint32_t>(Bytes[0]) |
                    (static_cast<uint32_t>(Bytes[1]) << 8) |
                    (static_cast<uint32_t>(Bytes[2]) << 16) |
                    (static_cast<uint32_t>(Bytes[3]) << 24);
    if (a64native::tryDecode(Word, Addr, *InsnBuf, *InsnBuf->detail)) {
      Out.Addr = InsnBuf->address;
      Out.Size = static_cast<uint16_t>(InsnBuf->size);
      Out.Id = InsnBuf->id;
      Out.Raw = InsnBuf;
      return Out.Size;
    }
  }
  return 0;
}

void Decoder::liftAArch64ToLow(
    const DecodedInsn &Insn, std::vector<LowOp> &Ops,
    llvm::ArrayRef<RelocatedScalarOperand> ScalarRelocs) {
  bool ScalarWideImmediate = false;
  if (Insn.Raw && Insn.Raw->size == 4) {
    uint32_t Word = 0;
    for (unsigned Byte = 0; Byte != 4; ++Byte)
      Word |= uint32_t(Insn.Raw->bytes[Byte]) << (Byte * 8);
    if ((Word & 0x1f800000u) == 0x12800000u)
      for (const RelocatedScalarOperand &Scalar : ScalarRelocs)
        ScalarWideImmediate |=
            Scalar.Semantics ==
                RelocatedScalarOperand::Kind::AArch64ELFUnrelocatedWideMove &&
            Scalar.FieldVA == Insn.Raw->address && Scalar.Width == 4 &&
            Scalar.EncodedValue == Word;
  }
  AArch64->lift(Insn.Raw, Ops, ScalarWideImmediate);
}

} // namespace neverd
