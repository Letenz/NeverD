//===- X86LegacyVectorMoveAudit.h - Legacy vector move audit ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Undefined-output admission for complete legacy 128-bit integer moves.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86LEGACYVECTORMOVEAUDIT_H
#define NEVERD_LIB_LIFT_X86_X86LEGACYVECTORMOVEAUDIT_H

#include "X86LegacyMemoryAudit.h"

namespace neverd::vectormoveaudit {

// Intel SDM Vol. 2B, MOVDQU/MOVDQA: all 128 destination bits are copied;
// legacy register writes preserve the higher vector bits and flags. This is
// only undefined-output coverage, not a preserved-opaque-bank audit. MOVDQA
// memory stays unaudited until its alignment fault has an explicit contract.
inline bool form(const cs_insn *Insn, Arch Target) {
  const auto &X = Insn->detail->x86;
  if ((Insn->id != X86_INS_MOVDQU && Insn->id != X86_INS_MOVDQA) ||
      X.op_count != 2 || Insn->size > 15)
    return false;
  const bool Long = Target == Arch::X64;
  const uint8_t RequiredPrefix = Insn->id == X86_INS_MOVDQU ? 0xf3 : 0x66;
  size_t Pos = 0;
  uint8_t MandatoryPrefix = 0, AddressPrefix = 0, Rex = 0;
  for (; Pos < Insn->size; ++Pos) {
    const uint8_t B = Insn->bytes[Pos];
    if (B == RequiredPrefix) {
      if (MandatoryPrefix || Rex)
        return false;
      MandatoryPrefix = B;
    } else if (B == 0x67) {
      if (AddressPrefix || Rex)
        return false;
      AddressPrefix = B;
    } else if (Long && B >= 0x40 && B <= 0x4f) {
      if (Rex)
        return false;
      Rex = B;
    } else {
      break;
    }
  }
  if (!MandatoryPrefix || Pos + 3 > Insn->size || Insn->bytes[Pos] != 0x0f)
    return false;
  const uint8_t Opcode = Insn->bytes[Pos + 1];
  const uint8_t ModRM = Insn->bytes[Pos + 2];
  const unsigned AddressSize =
      Long ? (AddressPrefix ? 4 : 8) : (AddressPrefix ? 2 : 4);
  // Capstone removes mandatory F3 from its normalized repeat-prefix field;
  // the raw prefix above remains authoritative for the instruction identity.
  if ((Opcode != 0x6f && Opcode != 0x7f) || X.modrm != ModRM ||
      X.encoding.modrm_offset != Pos + 2 || X.opcode[0] != 0x0f ||
      X.opcode[1] != Opcode || X.opcode[2] || X.opcode[3] || X.rex != Rex ||
      X.prefix[0] || X.prefix[1] ||
      X.prefix[2] != (RequiredPrefix == 0x66 ? 0x66 : 0) ||
      X.prefix[3] != AddressPrefix || X.addr_size != AddressSize ||
      X.encoding.imm_size || X.encoding.imm_offset || X.avx_sae ||
      X.avx_rm != X86_AVX_RM_INVALID || X.avx_cc != X86_AVX_CC_INVALID)
    return false;
  for (unsigned I = 0; I != 2; ++I)
    if (X.operands[I].size != 16 ||
        X.operands[I].avx_bcast != X86_AVX_BCAST_INVALID ||
        X.operands[I].avx_zero_opmask)
      return false;
  const auto Register = [&](const cs_x86_op &Operand, unsigned Number) {
    if (Operand.type != X86_OP_REG)
      return false;
    const auto R = mapCapstoneReg(static_cast<x86_reg>(Operand.reg));
    return R.Offset == x86reg::vectorReg(Number) && R.Size == 16;
  };
  const auto &Reg = X.operands[Opcode == 0x6f ? 0 : 1];
  const auto &RMOperand = X.operands[Opcode == 0x6f ? 1 : 0];
  if (!Register(Reg, ((ModRM >> 3) & 7) + ((Rex & 4) ? 8 : 0)))
    return false;
  if ((ModRM >> 6) == 3)
    return !AddressPrefix && !(Rex & 2) && Pos + 3 == Insn->size &&
           !X.encoding.disp_size && !X.encoding.disp_offset && !X.disp &&
           !X.sib && Register(RMOperand, (ModRM & 7) + ((Rex & 1) ? 8 : 0));
  return Insn->id == X86_INS_MOVDQU &&
         legacyaudit::memoryOperand(Insn, RMOperand, 16, AddressSize, Long, Rex,
                                    Pos + 3);
}

} // namespace neverd::vectormoveaudit

#endif
