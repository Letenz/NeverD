//===- X86BitTestUndefined.h - Register bit-test effect audit -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86BITTESTUNDEFINED_H
#define NEVERD_LIB_LIFT_X86_X86BITTESTUNDEFINED_H

#include "neverd/lift/X86Lifter.h"

#include <array>

namespace neverd::bitundefined {

inline bool isBitTest(unsigned Id) {
  return Id == X86_INS_BT || Id == X86_INS_BTS || Id == X86_INS_BTR ||
         Id == X86_INS_BTC;
}

// Intel SDM 093, Vol. 2A, BT/BTC/BTR/BTS, Flags Affected. CF is defined;
// ZF and DF are preserved. These four outputs are fresh bits, not bytes.
inline constexpr std::array<unsigned, 4> Flags = {x86reg::OF, x86reg::SF,
                                                  x86reg::AF, x86reg::PF};

inline void record(X86Lifter::LiftState &S) {
  for (const auto Flag : Flags)
    S.recordUndefinedBits(NdVar::reg(Flag, 1), 0, 1);
}

inline bool matches(llvm::ArrayRef<LowOp> Ops,
                    const LowInstructionUndefinedEffects &Effects) {
  if (Effects.Effects.size() != Flags.size())
    return false;
  for (size_t I = 0; I != Flags.size(); ++I) {
    const auto &E = Effects.Effects[I];
    if (E.Output != NdVar::reg(Flags[I], 1) || E.BitOffset != 0 ||
        E.BitCount != 1 || E.When || E.AfterOp == 0 || E.AfterOp > Ops.size())
      return false;
    for (const auto &Later : Ops.drop_front(E.AfterOp)) {
      if (Later.Output == E.Output)
        return false;
      for (unsigned J = 0; J < Later.NumInputs; ++J)
        if (Later.Inputs[J] == E.Output)
          return false;
    }
  }
  return true;
}

// Only register bit bases with a register or imm8 index. Memory bit strings,
// LOCK/REP, segment/address prefixes, duplicate prefixes and APX stay outside
// this audit. Raw operands and widths must agree with the encoded registers.
inline bool form(const cs_insn *Insn, Arch Target) {
  const auto &X = Insn->detail->x86;
  if (!isBitTest(Insn->id) || X.op_count != 2 || Insn->size > 15)
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
  const bool Immediate = Opcode == 0xba;
  const unsigned Kind = Insn->id == X86_INS_BT    ? 0
                        : Insn->id == X86_INS_BTS ? 1
                        : Insn->id == X86_INS_BTR ? 2
                                                  : 3;
  if ((Immediate ? ((ModRM >> 3) & 7) != Kind + 4 || (Rex & 4)
                 : Opcode != 0xa3 + Kind * 8) ||
      (ModRM >> 6) != 3 || Insn->size != Pos + 3 + Immediate ||
      X.modrm != ModRM || X.encoding.modrm_offset != Pos + 2 ||
      X.opcode[0] != 0x0f || X.opcode[1] != Opcode || X.opcode[2] ||
      X.opcode[3] || X.rex != Rex || X.prefix[0] || X.prefix[1] ||
      X.prefix[2] != OperandPrefix || X.prefix[3] ||
      X.addr_size != (Long ? 8 : 4) || X.encoding.disp_size ||
      X.encoding.disp_offset || X.encoding.imm_size != (Immediate ? 1 : 0) ||
      X.encoding.imm_offset != (Immediate ? Pos + 3 : 0))
    return false;
  const unsigned Width = (Rex & 8) ? 8 : OperandPrefix ? 2 : 4;
  const auto Register = [&](const cs_x86_op &Operand, unsigned Number) {
    if (Operand.type != X86_OP_REG || Operand.size != Width)
      return false;
    const auto R = mapCapstoneReg(static_cast<x86_reg>(Operand.reg));
    return R.Offset == Number * 8 && R.Size == Width;
  };
  if (!Register(X.operands[0], (ModRM & 7) + ((Rex & 1) ? 8 : 0)))
    return false;
  const auto &Index = X.operands[1];
  return Immediate ? Index.type == X86_OP_IMM && Index.size == 1 &&
                         Index.imm == Insn->bytes[Pos + 3]
                   : Register(Index, ((ModRM >> 3) & 7) + ((Rex & 4) ? 8 : 0));
}

} // namespace neverd::bitundefined

#endif
