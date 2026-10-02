//===- X86XaddAudit.h - Legacy register XADD effect audit -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86XADDAUDIT_H
#define NEVERD_LIB_LIFT_X86_X86XADDAUDIT_H

#include "neverd/lift/X86Lifter.h"

namespace neverd::xaddaudit {

// Intel SDM 093, Vol. 2D, XADD: the addition defines all six arithmetic
// flags, and DF is preserved. Audit exact legacy register forms only;
// memory/LOCK, APX, duplicate and unrelated prefixes remain unaudited.
inline bool form(const cs_insn *Insn, Arch Target) {
  const auto &X = Insn->detail->x86;
  if (Insn->id != X86_INS_XADD || X.op_count != 2 || Insn->size > 15)
    return false;
  const bool Long = Target == Arch::X64;
  size_t Pos = 0;
  uint8_t OperandPrefix = 0, Rex = 0;
  if (Pos < Insn->size && Insn->bytes[Pos] == 0x66)
    OperandPrefix = Insn->bytes[Pos++];
  if (Long && Pos < Insn->size && Insn->bytes[Pos] >= 0x40 &&
      Insn->bytes[Pos] <= 0x4f)
    Rex = Insn->bytes[Pos++];
  if (Pos + 3 != Insn->size || Insn->bytes[Pos] != 0x0f || (Rex & 2))
    return false;
  const uint8_t Opcode = Insn->bytes[Pos + 1];
  const uint8_t ModRM = Insn->bytes[Pos + 2];
  if ((Opcode != 0xc0 && Opcode != 0xc1) || (ModRM >> 6) != 3 ||
      X.modrm != ModRM || X.encoding.modrm_offset != Pos + 2 ||
      X.opcode[0] != 0x0f || X.opcode[1] != Opcode || X.opcode[2] ||
      X.opcode[3] || X.rex != Rex || X.prefix[0] || X.prefix[1] ||
      X.prefix[2] != OperandPrefix || X.prefix[3] ||
      X.addr_size != (Long ? 8 : 4) || X.encoding.disp_size ||
      X.encoding.disp_offset || X.encoding.imm_size || X.encoding.imm_offset)
    return false;
  const unsigned Width = Opcode == 0xc0  ? 1
                         : (Rex & 8)     ? 8
                         : OperandPrefix ? 2
                                         : 4;
  const auto Register = [&](const cs_x86_op &Operand, unsigned Number) {
    if (Operand.type != X86_OP_REG || Operand.size != Width)
      return false;
    const uint64_t Offset =
        Width == 1 && !Rex && Number >= 4 ? (Number - 4) * 8 + 1 : Number * 8;
    const auto R = mapCapstoneReg(static_cast<x86_reg>(Operand.reg));
    return R.Offset == Offset && R.Size == Width;
  };
  return Register(X.operands[0], (ModRM & 7) + ((Rex & 1) ? 8 : 0)) &&
         Register(X.operands[1], ((ModRM >> 3) & 7) + ((Rex & 4) ? 8 : 0));
}

} // namespace neverd::xaddaudit

#endif
