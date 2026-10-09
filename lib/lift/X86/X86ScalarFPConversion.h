//===- X86ScalarFPConversion.h - Scalar integer conversion state -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86SCALARFPCONVERSION_H
#define NEVERD_LIB_LIFT_X86_X86SCALARFPCONVERSION_H

#include "X86LiftDetail.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/X86FPState.h"
#include "neverd/lift/X86Lifter.h"

#include <array>

namespace neverd {
inline bool liftScalarFPIntegerState(X86Lifter &L, X86Lifter::LiftState &S,
                                     const cs_insn *Insn, const cs_x86 &X86,
                                     unsigned SourceBytes, bool Truncate,
                                     bool Vex) {
  const auto Refuse = [&]() -> bool {
    throw UnliftedInstruction(Insn ? Insn->address : InvalidVA,
                              Insn ? Insn->mnemonic : nullptr,
                              Insn ? Insn->op_str : nullptr);
  };
  if (!Insn || Insn->size == 0 || Insn->size > 15 ||
      (L.targetArch() != Arch::X64 && L.targetArch() != Arch::X86) ||
      (SourceBytes != 4 && SourceBytes != 8) || X86.op_count != 2 ||
      X86.operands[0].type != X86_OP_REG ||
      (X86.operands[0].size != 4 && X86.operands[0].size != 8) ||
      (X86.operands[1].type != X86_OP_REG &&
       X86.operands[1].type != X86_OP_MEM) ||
      X86.encoding.imm_offset || X86.encoding.imm_size)
    return Refuse();
  const bool Mode64 = L.targetArch() == Arch::X64;
  unsigned Offset = 0;
  uint8_t Rex = 0, Mandatory = 0, Segment = 0;
  bool Address = false, Operand = false;
  while (Offset < Insn->size && Offset < 15) {
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
    } else if (!Vex && (Byte == 0xf2 || Byte == 0xf3)) {
      if (Mandatory || Rex)
        return Refuse();
      Mandatory = Byte;
    } else if (!Vex && Mode64 && (Byte & 0xf0) == 0x40) {
      if (Rex)
        return Refuse();
      Rex = Byte;
    } else
      break;
    ++Offset;
  }
  bool W = (Rex & 8) != 0, R = (Rex & 4) != 0, B = (Rex & 1) != 0;
  bool X = (Rex & 2) != 0;
  if (Vex) {
    if (Offset + 3 >= Insn->size)
      return Refuse();
    uint8_t P1 = 0;
    if (Insn->bytes[Offset] == 0xc5) {
      P1 = Insn->bytes[Offset + 1];
      R = (P1 & 0x80) == 0;
      Offset += 2;
    } else if (Insn->bytes[Offset] == 0xc4) {
      if (Offset + 4 >= Insn->size || (Insn->bytes[Offset + 1] & 0x1f) != 1)
        return Refuse();
      const auto P0 = Insn->bytes[Offset + 1];
      P1 = Insn->bytes[Offset + 2];
      R = (P0 & 0x80) == 0;
      B = (P0 & 0x20) == 0;
      X = (P0 & 0x40) == 0;
      W = (P1 & 0x80) != 0;
      if (!Mode64 && (P0 & 0xe0) != 0xe0)
        return Refuse();
      Offset += 3;
    } else
      return Refuse();
    if ((!Mode64 && R) || (P1 & 0x7c) != 0x78 ||
        (P1 & 3) != (SourceBytes == 4 ? 2 : 3))
      return Refuse();
  } else {
    if (Mandatory != (SourceBytes == 4 ? 0xf3 : 0xf2) ||
        Offset + 2 >= Insn->size || Insn->bytes[Offset++] != 0x0f)
      return Refuse();
  }
  if (Offset + 1 >= Insn->size ||
      Insn->bytes[Offset] != (Truncate ? 0x2c : 0x2d) ||
      X86.encoding.modrm_offset != Offset + 1 ||
      Insn->bytes[Offset + 1] != X86.modrm)
    return Refuse();
  const unsigned DestinationBytes = Mode64 && W ? 8 : 4;
  if (X86.operands[0].size != DestinationBytes)
    return Refuse();
  constexpr std::array<unsigned, 16> GPR32 = {
      X86_REG_EAX,  X86_REG_ECX,  X86_REG_EDX,  X86_REG_EBX,
      X86_REG_ESP,  X86_REG_EBP,  X86_REG_ESI,  X86_REG_EDI,
      X86_REG_R8D,  X86_REG_R9D,  X86_REG_R10D, X86_REG_R11D,
      X86_REG_R12D, X86_REG_R13D, X86_REG_R14D, X86_REG_R15D};
  constexpr std::array<unsigned, 16> GPR64 = {
      X86_REG_RAX, X86_REG_RCX, X86_REG_RDX, X86_REG_RBX,
      X86_REG_RSP, X86_REG_RBP, X86_REG_RSI, X86_REG_RDI,
      X86_REG_R8,  X86_REG_R9,  X86_REG_R10, X86_REG_R11,
      X86_REG_R12, X86_REG_R13, X86_REG_R14, X86_REG_R15};
  const unsigned DestinationReg = ((X86.modrm >> 3) & 7) | (R ? 8 : 0);
  if (X86.operands[0].reg !=
      (DestinationBytes == 8 ? GPR64[DestinationReg] : GPR32[DestinationReg]))
    return Refuse();
  if ((X86.modrm >> 6) == 3) {
    const unsigned SourceReg = (X86.modrm & 7) | (B ? 8 : 0);
    if (X86.operands[1].type != X86_OP_REG ||
        X86.operands[1].reg != X86_REG_XMM0 + SourceReg ||
        (X86.operands[1].size != 16 && X86.operands[1].size != SourceBytes))
      return Refuse();
  } else if (X86.operands[1].type != X86_OP_MEM ||
             X86.operands[1].size != SourceBytes)
    return Refuse();
  const unsigned AddressSize = Mode64 ? (Address ? 4 : 8) : (Address ? 2 : 4);
  if (!validateCanonicalScalarConversionTail(Insn, X86, Offset + 2, Segment,
                                             Mode64, AddressSize, B ? 8 : 0,
                                             X ? 8 : 0, X86.operands[1]))
    return Refuse();
  const auto Destination = L.operandWrite(X86.operands[0]);
  auto Source = L.operandRead(S, X86.operands[1]);
  if (Source.Size > SourceBytes) {
    const auto Narrow = S.makeTemp(SourceBytes);
    S.emit(NdOp::SUBBYTES, Narrow, {Source, NdVar::cst(0, 4)});
    Source = Narrow;
  }
  const auto State = S.makeTemp(4);
  S.emitIntrinsic(Intrinsic::X86ReadMXCSR, State, {});
  const auto Completed = S.makeTemp(DestinationBytes + 4);
  S.emitIntrinsic(Truncate ? Intrinsic::X86FPTruncToIntState
                           : Intrinsic::X86FPCvtToIntState,
                  Completed, {Source, State, NdVar::cst(DestinationBytes, 4)});
  const auto Number = S.makeTemp(DestinationBytes);
  const auto Outgoing = S.makeTemp(4);
  S.emit(NdOp::SUBBYTES, Number, {Completed, NdVar::cst(0, 4)});
  S.emit(NdOp::SUBBYTES, Outgoing,
         {Completed, NdVar::cst(DestinationBytes, 4)});
  S.emit(NdOp::COPY, Destination, {Number});
  S.emitIntrinsic(Intrinsic::X86WriteMXCSR, {}, {Outgoing});
  return true;
}
} // namespace neverd
#endif
