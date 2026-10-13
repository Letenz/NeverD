//===- X86FPMaskCompareState.h - Authenticated CMP lifting ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve SSE/VEX comparison predicates, masks and MXCSR completion.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LIB_LIFT_X86_X86FPMASKCOMPARESTATE_H
#define NEVERD_LIB_LIFT_X86_X86FPMASKCOMPARESTATE_H

#include "X86LiftDetail.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/X86FPState.h"
#include "neverd/lift/X86Lifter.h"

namespace neverd {
/// Distinguish scalar CMP from string CMPSD before trusting decoded operands.
inline bool hasLegacyFPMaskCompareOpcode(const cs_insn *Insn) {
  if (!Insn || !Insn->size || Insn->size > 15)
    return false;
  unsigned I = 0;
  for (; I < Insn->size; ++I) {
    const auto B = Insn->bytes[I];
    if (!(B == 0x26 || B == 0x2e || B == 0x36 || B == 0x3e || B == 0x64 ||
          B == 0x65 || B == 0x66 || B == 0x67 || B == 0xf2 || B == 0xf3 ||
          (B & 0xf0) == 0x40))
      break;
  }
  return I + 1 < Insn->size && Insn->bytes[I] == 0x0f &&
         Insn->bytes[I + 1] == 0xc2;
}
inline bool liftFPMaskCompareState(X86Lifter &L, X86Lifter::LiftState &S,
                                   const cs_insn *Insn, const cs_x86 &X86,
                                   bool Double, bool Scalar, bool Vex) {
  const auto Refuse = [&]() -> bool {
    throw UnliftedInstruction(Insn ? Insn->address : InvalidVA,
                              Insn ? Insn->mnemonic : nullptr,
                              Insn ? Insn->op_str : nullptr);
  };
  const unsigned SourceIndex = Vex ? 2 : 1;
  if (!Insn || !Insn->size || Insn->size > 15 ||
      (L.targetArch() != Arch::X64 && L.targetArch() != Arch::X86) ||
      (X86.op_count != SourceIndex + 1 && X86.op_count != SourceIndex + 2) ||
      X86.operands[0].type != X86_OP_REG || X86.avx_sae ||
      X86.avx_rm != X86_AVX_RM_INVALID || X86.encoding.imm_size != 1 ||
      X86.encoding.imm_offset != Insn->size - 1)
    return Refuse();
  for (unsigned I = 0; I < X86.op_count; ++I)
    if (X86.operands[I].avx_bcast != X86_AVX_BCAST_INVALID ||
        X86.operands[I].avx_zero_opmask)
      return Refuse();
  const unsigned Immediate = Insn->bytes[Insn->size - 1];
  if (X86.op_count == SourceIndex + 2 &&
      (X86.operands[SourceIndex + 1].type != X86_OP_IMM ||
       X86.operands[SourceIndex + 1].imm != Immediate))
    return Refuse();
  const unsigned Control = (Double ? 1 : 0) | (Scalar ? 2 : 0) | (Vex ? 4 : 0);
  const unsigned Predicate = x86FPMaskComparePredicate(Control, Immediate);
  if (Vex ? X86.sse_cc != X86_SSE_CC_INVALID ||
                (X86.avx_cc != X86_AVX_CC_INVALID &&
                 unsigned(X86.avx_cc) != Predicate + 1)
          : X86.avx_cc != X86_AVX_CC_INVALID ||
                (X86.sse_cc != X86_SSE_CC_INVALID &&
                 unsigned(X86.sse_cc) != Predicate + 1))
    return Refuse();
  const bool Mode64 = L.targetArch() == Arch::X64;
  const unsigned PP = Scalar ? (Double ? 3 : 2) : (Double ? 1 : 0);
  const uint8_t Prefix = PP == 1 ? 0x66 : PP == 2 ? 0xf3 : PP == 3 ? 0xf2 : 0;
  unsigned Offset = 0, Width = 16;
  uint8_t Segment = 0, Rex = 0;
  bool Address = false, Mandatory = false;
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
    } else if (!Vex && Prefix && Byte == Prefix) {
      if (Mandatory || Rex)
        return Refuse();
      Mandatory = true;
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
      B = X = false;
      Offset += 2;
    } else if (Insn->bytes[Offset] == 0xc4) {
      if (Offset + 4 >= Insn->size)
        return Refuse();
      const auto P0 = Insn->bytes[Offset + 1];
      if ((P0 & 31) != 1 || (!Mode64 && (P0 & 0xe0) != 0xe0))
        return Refuse();
      R = !(P0 & 0x80);
      B = !(P0 & 0x20);
      X = !(P0 & 0x40);
      P1 = Insn->bytes[Offset + 2];
      Offset += 3;
    } else
      return Refuse();
    if ((P1 & 3) != PP || (Scalar && (P1 & 4)) || (!Mode64 && R))
      return Refuse();
    Width = P1 & 4 ? 32 : 16;
    const unsigned Left = (~P1 >> 3) & 15;
    if ((!Mode64 && Left >= 8) || X86.operands[1].type != X86_OP_REG ||
        X86.operands[1].size != Width ||
        X86.operands[1].reg !=
            (Width == 32 ? X86_REG_YMM0 : X86_REG_XMM0) + Left)
      return Refuse();
  } else {
    if (Mandatory != bool(Prefix) || Offset + 2 >= Insn->size ||
        Insn->bytes[Offset++] != 0x0f)
      return Refuse();
  }
  if (Offset + 1 >= Insn->size || Insn->bytes[Offset] != 0xc2 ||
      X86.encoding.modrm_offset != Offset + 1 ||
      X86.modrm != Insn->bytes[Offset + 1])
    return Refuse();
  const unsigned Base = Width == 32 ? X86_REG_YMM0 : X86_REG_XMM0;
  if (X86.operands[0].size != Width ||
      X86.operands[0].reg != Base + ((X86.modrm >> 3) & 7) + (R ? 8 : 0))
    return Refuse();
  const unsigned Element = Double ? 8 : 4, Bytes = Scalar ? Element : Width;
  const auto &RHS = X86.operands[SourceIndex];
  const bool Memory = (X86.modrm >> 6) != 3;
  if (Memory ? RHS.type != X86_OP_MEM || RHS.size != Bytes
             : RHS.type != X86_OP_REG || RHS.size != Width ||
                   RHS.reg != Base + (X86.modrm & 7) + (B ? 8 : 0))
    return Refuse();
  if (!validateCanonicalScalarConversionTail(
          Insn, X86, Offset + 2, Segment, Mode64,
          Mode64 ? (Address ? 4 : 8) : (Address ? 2 : 4), B ? 8 : 0, X ? 8 : 0,
          RHS, 1))
    return Refuse();
  const auto Slice = [&](NdVar Input) {
    if (Input.Size == Bytes)
      return Input;
    const auto Narrow = S.makeTemp(Bytes);
    S.emit(NdOp::SUBBYTES, Narrow, {Input, NdVar::cst(0, 4)});
    return Narrow;
  };
  const auto LeftFull = L.operandRead(S, X86.operands[Vex ? 1 : 0]);
  const auto Left = Slice(LeftFull);
  const auto Right = Memory ? S.computeEA(RHS) : Slice(L.operandRead(S, RHS));
  const auto Incoming = S.makeTemp(4), Completed = S.makeTemp(Bytes + 4);
  S.emitIntrinsic(Intrinsic::X86ReadMXCSR, Incoming);
  if (Memory)
    S.emitIntrinsic(Intrinsic::X86FPMaskCompareMemoryState, Completed,
                    {Right, NdVar::cst(Control, 1), NdVar::cst(Immediate, 1),
                     Left, Incoming},
                    NdMemoryOrdering::None, S.memoryAddressSpace(RHS));
  else
    S.emitIntrinsic(Intrinsic::X86FPMaskCompareState, Completed,
                    {NdVar::cst(Control, 1), NdVar::cst(Immediate, 1), Left,
                     Right, Incoming});
  const auto Mask = S.makeTemp(Bytes), State = S.makeTemp(4);
  S.emit(NdOp::SUBBYTES, Mask, {Completed, NdVar::cst(0, 4)});
  S.emit(NdOp::SUBBYTES, State, {Completed, NdVar::cst(Bytes, 4)});
  S.emitVoidIntrinsic(Intrinsic::X86WriteMXCSR, {State});
  const auto Destination = L.operandWrite(X86.operands[0]);
  if (Scalar) {
    const auto Upper = S.makeTemp(16 - Element);
    S.emit(NdOp::SUBBYTES, Upper, {LeftFull, NdVar::cst(Element, 4)});
    S.emit(NdOp::CONCAT, Destination, {Upper, Mask});
  } else
    S.emit(NdOp::COPY, Destination, {Mask});
  return true;
}
} // namespace neverd
#endif // NEVERD_LIB_LIFT_X86_X86FPMASKCOMPARESTATE_H
