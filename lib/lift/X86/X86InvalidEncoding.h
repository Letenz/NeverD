//===- X86InvalidEncoding.h - Invalid x86 operand/prefix forms -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86INVALIDENCODING_H
#define NEVERD_LIB_LIFT_X86_X86INVALIDENCODING_H

#include "neverd/Common.h"

#include "capstone/capstone.h"

namespace neverd {

/// Capstone may accept an ignored LOCK or MOV-to-CS encoding. Neither has
/// the ordinary instruction's semantics: the processor raises #UD. Reject
/// before reading memory, publishing state or updating inter-instruction facts.
inline bool hasInvalidX86SemanticEncoding(const cs_insn &Insn, Arch Target) {
  const auto &X86 = Insn.detail->x86;
  if (Insn.id == X86_INS_MOV && X86.op_count &&
      X86.operands[0].type == X86_OP_REG && X86.operands[0].reg == X86_REG_CS)
    return true;
  bool Locked = false;
  for (unsigned I = 0; I < Insn.size; ++I) {
    const uint8_t Byte = Insn.bytes[I];
    if (Byte == 0xf0) {
      Locked = true;
      continue;
    }
    if (Byte == 0xf2 || Byte == 0xf3 || Byte == 0x66 || Byte == 0x67 ||
        Byte == 0x26 || Byte == 0x2e || Byte == 0x36 || Byte == 0x3e ||
        Byte == 0x64 || Byte == 0x65 ||
        (Target == Arch::X64 && (Byte & 0xf0) == 0x40))
      continue;
    break;
  }
  if (!Locked)
    return false;
  if (!X86.op_count || X86.operands[0].type != X86_OP_MEM)
    return true;
  switch (Insn.id) {
  case X86_INS_ADD:
  case X86_INS_ADC:
  case X86_INS_AND:
  case X86_INS_BTC:
  case X86_INS_BTR:
  case X86_INS_BTS:
  case X86_INS_CMPXCHG:
  case X86_INS_CMPXCHG8B:
  case X86_INS_CMPXCHG16B:
  case X86_INS_DEC:
  case X86_INS_INC:
  case X86_INS_NEG:
  case X86_INS_NOT:
  case X86_INS_OR:
  case X86_INS_SBB:
  case X86_INS_SUB:
  case X86_INS_XOR:
  case X86_INS_XADD:
  case X86_INS_XCHG:
    return false;
  default:
    return true;
  }
}

} // namespace neverd

#endif
