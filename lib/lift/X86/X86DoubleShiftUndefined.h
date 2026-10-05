//===- X86DoubleShiftUndefined.h - SHLD/SHRD effect audit -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86DOUBLESHIFTUNDEFINED_H
#define NEVERD_LIB_LIFT_X86_X86DOUBLESHIFTUNDEFINED_H

#include "X86ShiftUndefined.h"

namespace neverd::doubleshiftundefined {

inline bool isDoubleShift(unsigned Id) {
  return Id == X86_INS_SHLD || Id == X86_INS_SHRD;
}

// Intel SDM 093, Vol. 2B, SHLD/SHRD. A count equal to the operand width is
// defined; only a greater masked count makes the result and CF/ZF/SF/PF
// arbitrary. That case is possible only at word width. AF/OF have their
// separate thresholds, and DF and all other register bits remain preserved.
inline auto rules(NdVar Destination) {
  using shiftundefined::Rule;
  const unsigned Excess = Destination.Size * 8 + 1;
  return std::array<Rule, 7>{{
      {NdVar::reg(x86reg::AF, 1), 1, 1},
      {NdVar::reg(x86reg::OF, 1), 1, 2},
      {NdVar::reg(x86reg::CF, 1), 1, Excess},
      {NdVar::reg(x86reg::PF, 1), 1, Excess},
      {NdVar::reg(x86reg::ZF, 1), 1, Excess},
      {NdVar::reg(x86reg::SF, 1), 1, Excess},
      {Destination, unsigned(Destination.Size) * 8, Excess},
  }};
}

inline void record(X86Lifter::LiftState &S, NdVar Count, NdVar Destination) {
  shiftundefined::recordRules(S, Count, rules(Destination));
}

inline bool matches(const cs_x86 &X, llvm::ArrayRef<LowOp> Ops,
                    const LowInstructionUndefinedEffects &Effects) {
  if (X.op_count != 3 || X.operands[0].type != X86_OP_REG)
    return false;
  const auto R = mapCapstoneReg(X.operands[0].reg);
  return shiftundefined::matchesRules(
      X.operands[0].size, X.operands[2], NdVar::reg(x86reg::RCX, 1),
      rules(NdVar::reg(R.Offset, R.Size)), Ops, Effects);
}

// Start with exact legacy register encodings. Memory, LOCK/REP, APX,
// address/segment overrides, duplicate prefixes and unused REX.X stay outside
// this audit. Operand and immediate details must match the original bytes.
inline bool form(const cs_insn *Insn, Arch Target) {
  const auto &X = Insn->detail->x86;
  if (!isDoubleShift(Insn->id) || X.op_count != 3 || Insn->size > 15)
    return false;
  const bool Long = Target == Arch::X64;
  size_t Pos = 0;
  uint8_t OperandPrefix = 0, Rex = 0;
  if (Pos < Insn->size && Insn->bytes[Pos] == 0x66)
    OperandPrefix = Insn->bytes[Pos++];
  if (Long && Pos < Insn->size && Insn->bytes[Pos] >= 0x40 &&
      Insn->bytes[Pos] <= 0x4f)
    Rex = Insn->bytes[Pos++];
  if (Pos + 3 > Insn->size || Insn->bytes[Pos] != 0x0f || (Rex & 2))
    return false;
  const uint8_t Opcode = Insn->bytes[Pos + 1];
  const uint8_t ModRM = Insn->bytes[Pos + 2];
  const bool Immediate = Opcode == 0xa4 || Opcode == 0xac;
  const bool Right = Insn->id == X86_INS_SHRD;
  if ((Opcode != (Right ? 0xac : 0xa4) && Opcode != (Right ? 0xad : 0xa5)) ||
      Pos + 3 + Immediate != Insn->size || (ModRM >> 6) != 3 ||
      X.modrm != ModRM || X.encoding.modrm_offset != Pos + 2 ||
      X.opcode[0] != 0x0f || X.opcode[1] != Opcode || X.opcode[2] ||
      X.opcode[3] || X.rex != Rex || X.prefix[0] || X.prefix[1] ||
      X.prefix[2] != OperandPrefix || X.prefix[3] ||
      X.addr_size != (Long ? 8 : 4) || X.encoding.disp_size ||
      X.encoding.disp_offset || X.encoding.imm_size != unsigned(Immediate) ||
      X.encoding.imm_offset != (Immediate ? Pos + 3 : 0))
    return false;
  const unsigned Width = (Rex & 8) ? 8 : OperandPrefix ? 2 : 4;
  const auto Register = [&](const cs_x86_op &Operand, unsigned Number) {
    if (Operand.type != X86_OP_REG || Operand.size != Width)
      return false;
    const auto R = mapCapstoneReg(Operand.reg);
    return R.Offset == Number * 8 && R.Size == Width;
  };
  const auto &Count = X.operands[2];
  return Register(X.operands[0], (ModRM & 7) + ((Rex & 1) ? 8 : 0)) &&
         Register(X.operands[1], ((ModRM >> 3) & 7) + ((Rex & 4) ? 8 : 0)) &&
         Count.size == 1 &&
         (Immediate
              ? Count.type == X86_OP_IMM && Count.imm == Insn->bytes[Pos + 3]
              : Count.type == X86_OP_REG && Count.reg == X86_REG_CL);
}

} // namespace neverd::doubleshiftundefined

#endif
