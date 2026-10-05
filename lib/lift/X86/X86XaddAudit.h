//===- X86XaddAudit.h - Legacy XADD effect audit ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86XADDAUDIT_H
#define NEVERD_LIB_LIFT_X86_X86XADDAUDIT_H

#include "X86LiftDetail.h"

namespace neverd::xaddaudit {

// Intel SDM, Vol. 2D, XADD: the addition defines all six arithmetic
// flags, and DF is preserved. Audit legacy register and unsegmented, non-LOCK
// memory forms. LOCK, APX, duplicate and unrelated prefixes remain unaudited.
inline bool form(const cs_insn *Insn, Arch Target) {
  const auto &X = Insn->detail->x86;
  if (Insn->id != X86_INS_XADD || X.op_count != 2 || Insn->size > 15)
    return false;
  const bool Long = Target == Arch::X64;
  size_t Pos = 0;
  uint8_t OperandPrefix = 0, AddressPrefix = 0, Rex = 0;
  for (; Pos < Insn->size; ++Pos) {
    const uint8_t B = Insn->bytes[Pos];
    if (B == 0x66) {
      if (OperandPrefix || Rex)
        return false;
      OperandPrefix = B;
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
  if (Pos + 3 > Insn->size || Insn->bytes[Pos] != 0x0f)
    return false;
  const uint8_t Opcode = Insn->bytes[Pos + 1];
  const uint8_t ModRM = Insn->bytes[Pos + 2];
  const unsigned AddressSize =
      Long ? (AddressPrefix ? 4 : 8) : (AddressPrefix ? 2 : 4);
  if ((Opcode != 0xc0 && Opcode != 0xc1) || X.modrm != ModRM ||
      X.encoding.modrm_offset != Pos + 2 || X.opcode[0] != 0x0f ||
      X.opcode[1] != Opcode || X.opcode[2] || X.opcode[3] || X.rex != Rex ||
      X.prefix[0] || X.prefix[1] || X.prefix[2] != OperandPrefix ||
      X.prefix[3] != AddressPrefix || X.addr_size != AddressSize ||
      X.encoding.imm_size || X.encoding.imm_offset)
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
  if (!Register(X.operands[1], ((ModRM >> 3) & 7) + ((Rex & 4) ? 8 : 0)))
    return false;
  const unsigned Mod = ModRM >> 6, RM = ModRM & 7;
  if (Mod == 3)
    return !AddressPrefix && !(Rex & 2) && Pos + 3 == Insn->size &&
           !X.encoding.disp_size && !X.encoding.disp_offset &&
           Register(X.operands[0], RM + ((Rex & 1) ? 8 : 0));

  const auto &D = X.operands[0];
  if (D.type != X86_OP_MEM || D.size != Width ||
      D.mem.segment != X86_REG_INVALID)
    return false;
  Pos += 3;
  unsigned DispSize = Mod == 1 ? 1 : Mod == 2 ? (AddressSize == 2 ? 2 : 4) : 0;
  uint8_t Sib = 0;
  if (AddressSize == 2) {
    constexpr x86_reg Bases[] = {X86_REG_BX, X86_REG_BX, X86_REG_BP,
                                 X86_REG_BP, X86_REG_SI, X86_REG_DI,
                                 X86_REG_BP, X86_REG_BX};
    constexpr x86_reg Indices[] = {
        X86_REG_SI,      X86_REG_DI,      X86_REG_SI,      X86_REG_DI,
        X86_REG_INVALID, X86_REG_INVALID, X86_REG_INVALID, X86_REG_INVALID};
    const bool Absolute = Mod == 0 && RM == 6;
    if (Absolute)
      DispSize = 2;
    if (D.mem.base != (Absolute ? X86_REG_INVALID : Bases[RM]) ||
        D.mem.index != Indices[RM] || D.mem.scale != 1)
      return false;
  } else {
    int Base = RM + ((Rex & 1) ? 8 : 0), Index = -1;
    unsigned Scale = 1;
    bool Relative = false;
    if (RM == 4) {
      if (Pos == Insn->size)
        return false;
      Sib = Insn->bytes[Pos++];
      Scale = 1U << (Sib >> 6);
      Index = ((Sib >> 3) & 7) + ((Rex & 2) ? 8 : 0);
      if (Index == 4)
        Index = -1;
      Base = (Sib & 7) + ((Rex & 1) ? 8 : 0);
      if (Mod == 0 && (Sib & 7) == 5) {
        Base = -1;
        DispSize = 4;
      }
    } else if (Mod == 0 && RM == 5) {
      Base = -1;
      Relative = Long;
      DispSize = 4;
    }
    const auto AddressRegister = [&](x86_reg R, int Number) {
      const auto Info = mapCapstoneReg(R);
      return Info.Offset == uint64_t(Number) * 8 && Info.Size == AddressSize;
    };
    const bool BaseMatches =
        Relative ? D.mem.base == (AddressSize == 8 ? X86_REG_RIP : X86_REG_EIP)
        : Base < 0 ? D.mem.base == X86_REG_INVALID
                   : AddressRegister(D.mem.base, Base);
    if (!BaseMatches ||
        (Index < 0 ? !isNoSibIndex(D.mem.index, AddressSize)
                   : !AddressRegister(D.mem.index, Index)) ||
        D.mem.scale != int(Scale))
      return false;
  }
  if (X.sib != Sib || Pos + DispSize != Insn->size ||
      X.encoding.disp_size != DispSize ||
      X.encoding.disp_offset != (DispSize ? Pos : 0))
    return false;
  uint64_t RawDisplacement = 0;
  for (unsigned I = 0; I != DispSize; ++I)
    RawDisplacement |= uint64_t(Insn->bytes[Pos + I]) << (I * 8);
  const int64_t Displacement =
      DispSize && (RawDisplacement >> (DispSize * 8 - 1))
          ? int64_t(RawDisplacement) - (INT64_C(1) << (DispSize * 8))
          : int64_t(RawDisplacement);
  return X.disp == Displacement && D.mem.disp == Displacement;
}

} // namespace neverd::xaddaudit

#endif
