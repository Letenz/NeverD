//===- X86FPComiState.h - Authenticated scalar comparison lifting --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Retain raw operand roles and one flags/MXCSR completion for SSE/VEX COMI.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LIB_LIFT_X86_X86FPCOMISTATE_H
#define NEVERD_LIB_LIFT_X86_X86FPCOMISTATE_H
#include "X86LiftDetail.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/X86FPState.h"
#include "neverd/lift/X86Lifter.h"
namespace neverd {
inline bool liftFPComiState(X86Lifter &L, X86Lifter::LiftState &S,
                            const cs_insn *Insn, const cs_x86 &X86, bool Double,
                            bool Quiet, bool Vex) {
  const auto Refuse = [&]() -> bool {
    throw UnliftedInstruction(Insn ? Insn->address : InvalidVA,
                              Insn ? Insn->mnemonic : nullptr,
                              Insn ? Insn->op_str : nullptr);
  };
  if (!Insn || !Insn->size || Insn->size > 15 || X86.op_count != 2 ||
      (L.targetArch() != Arch::X64 && L.targetArch() != Arch::X86) ||
      X86.operands[0].type != X86_OP_REG || X86.operands[0].size != 16 ||
      X86.encoding.imm_offset || X86.encoding.imm_size || X86.avx_sae ||
      X86.avx_rm != X86_AVX_RM_INVALID)
    return Refuse();
  const bool Mode64 = L.targetArch() == Arch::X64;
  unsigned Offset = 0;
  uint8_t Rex = 0, Segment = 0;
  bool Address = false, Operand = false;
  while (Offset < Insn->size) {
    const auto Byte = Insn->bytes[Offset];
    if (Byte == 0x26 || Byte == 0x2e || Byte == 0x36 || Byte == 0x3e ||
        Byte == 0x64 || Byte == 0x65) {
      if (Segment || Rex)
        return Refuse();
      Segment = Byte;
    } else if (Byte == 0x67) {
      if (Address || Rex)
        return Refuse();
      Address = true;
    } else if (!Vex && Byte == 0x66) {
      if (Operand || Rex)
        return Refuse();
      Operand = true;
    } else if (!Vex && Mode64 && (Byte & 0xf0) == 0x40) {
      if (Rex)
        return Refuse();
      Rex = Byte;
    } else
      break;
    ++Offset;
  }
  bool R = Rex & 4, B = Rex & 1, X = Rex & 2;
  if (Vex) {
    if (Offset + 3 >= Insn->size)
      return Refuse();
    uint8_t P1;
    if (Insn->bytes[Offset] == 0xc5) {
      P1 = Insn->bytes[Offset + 1];
      R = !(P1 & 0x80);
      Offset += 2;
    } else if (Insn->bytes[Offset] == 0xc4) {
      if (Offset + 4 >= Insn->size || (Insn->bytes[Offset + 1] & 31) != 1)
        return Refuse();
      const auto P0 = Insn->bytes[Offset + 1];
      P1 = Insn->bytes[Offset + 2];
      R = !(P0 & 0x80);
      B = !(P0 & 0x20);
      X = !(P0 & 0x40);
      Offset += 3;
    } else
      return Refuse();
    if ((P1 & 0x7f) != (Double ? 0x79 : 0x78) || (!Mode64 && (R || B || X)))
      return Refuse();
  } else if (Operand != Double || Offset + 2 >= Insn->size ||
             Insn->bytes[Offset++] != 0x0f)
    return Refuse();
  if (Offset + 1 >= Insn->size ||
      Insn->bytes[Offset] != (Quiet ? 0x2e : 0x2f) ||
      X86.encoding.modrm_offset != Offset + 1 ||
      X86.modrm != Insn->bytes[Offset + 1])
    return Refuse();
  const unsigned LeftReg = ((X86.modrm >> 3) & 7) | (R ? 8 : 0);
  if (X86.operands[0].reg != X86_REG_XMM0 + LeftReg)
    return Refuse();
  const unsigned Bytes = Double ? 8 : 4;
  const auto &RightOp = X86.operands[1];
  const bool Memory = (X86.modrm >> 6) != 3;
  if (Memory
          ? RightOp.type != X86_OP_MEM || RightOp.size != Bytes
          : RightOp.type != X86_OP_REG || RightOp.size != 16 ||
                RightOp.reg != X86_REG_XMM0 + ((X86.modrm & 7) | (B ? 8 : 0)))
    return Refuse();
  if (!validateCanonicalScalarConversionTail(
          Insn, X86, Offset + 2, Segment, Mode64,
          Mode64 ? (Address ? 4 : 8) : (Address ? 2 : 4), B ? 8 : 0, X ? 8 : 0,
          RightOp))
    return Refuse();
  const auto Scalar = [&](const cs_x86_op &Op) {
    const auto Value = L.operandRead(S, Op), Narrow = S.makeTemp(Bytes);
    S.emit(NdOp::SUBBYTES, Narrow, {Value, NdVar::cst(0, 4)});
    return Narrow;
  };
  const auto Left = Scalar(X86.operands[0]);
  const auto Right = Memory ? S.computeEA(RightOp) : Scalar(RightOp);
  const auto Incoming = S.makeTemp(4), Completed = S.makeTemp(8);
  const unsigned Control = (Quiet ? 1 : 0) | (Double ? 2 : 0) | (Vex ? 4 : 0);
  S.emitIntrinsic(Intrinsic::X86ReadMXCSR, Incoming);
  if (Memory)
    S.emitIntrinsic(Intrinsic::X86FPComiMemoryState, Completed,
                    {Right, NdVar::cst(Control, 1), Left, Incoming},
                    NdMemoryOrdering::None, S.memoryAddressSpace(RightOp));
  else
    S.emitIntrinsic(Intrinsic::X86FPComiState, Completed,
                    {NdVar::cst(Control, 1), Left, Right, Incoming});
  const auto Flags = S.makeTemp(4), Outgoing = S.makeTemp(4);
  S.emit(NdOp::SUBBYTES, Flags, {Completed, NdVar::cst(0, 4)});
  S.emit(NdOp::SUBBYTES, Outgoing, {Completed, NdVar::cst(4, 4)});
  S.emitVoidIntrinsic(Intrinsic::X86WriteMXCSR, {Outgoing});
  for (const auto [Reg, Shift] :
       {std::pair{x86reg::CF, 0}, std::pair{x86reg::PF, 2},
        std::pair{x86reg::ZF, 6}}) {
    const auto Bit = S.makeTemp(4), Byte = S.makeTemp(1);
    S.emit(NdOp::INT_RIGHT, Bit, {Flags, NdVar::cst(Shift, 4)});
    S.emit(NdOp::SUBBYTES, Byte, {Bit, NdVar::cst(0, 4)});
    S.emit(NdOp::INT_AND, NdVar::reg(Reg, 1), {Byte, NdVar::cst(1, 1)});
  }
  for (auto Reg : {x86reg::OF, x86reg::SF, x86reg::AF})
    S.emit(NdOp::COPY, NdVar::reg(Reg, 1), {NdVar::cst(0, 1)});
  return true;
}
} // namespace neverd
#endif
