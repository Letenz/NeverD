//===- X86NativeStateAudit.h - Native state-effect audit -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Undefined-output admission for bounded string and control-state owners.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86NATIVESTATEAUDIT_H
#define NEVERD_LIB_LIFT_X86_X86NATIVESTATEAUDIT_H

#include "X86LegacyMemoryAudit.h"

namespace neverd::nativestateaudit {

inline bool cpuQueryForm(const cs_insn *Insn, Arch Target) {
  if (!Insn || !Insn->detail || Target != Arch::X64 ||
      Insn->id != X86_INS_CPUID || Insn->size != 2 || Insn->bytes[0] != 0x0f ||
      Insn->bytes[1] != 0xa2)
    return false;
  const auto &X = Insn->detail->x86;
  return !X.prefix[0] && !X.prefix[1] && !X.prefix[2] && !X.prefix[3] &&
         X.opcode[0] == 0x0f && X.opcode[1] == 0xa2 && !X.opcode[2] &&
         !X.opcode[3] && !X.rex && X.addr_size == 8 && !X.modrm && !X.sib &&
         !X.disp && !X.encoding.modrm_offset && !X.encoding.disp_offset &&
         !X.encoding.disp_size && !X.encoding.imm_offset &&
         !X.encoding.imm_size && !X.op_count && !X.avx_sae &&
         X.avx_rm == X86_AVX_RM_INVALID && X.avx_cc == X86_AVX_CC_INVALID &&
         !X.eflags;
}

inline bool swapgsForm(const cs_insn *Insn, Arch Target) {
  if (!Insn || !Insn->detail || Target != Arch::X64 ||
      Insn->id != X86_INS_SWAPGS || Insn->size != 3 || Insn->bytes[0] != 0x0f ||
      Insn->bytes[1] != 0x01 || Insn->bytes[2] != 0xf8)
    return false;
  const auto &X = Insn->detail->x86;
  return !X.prefix[0] && !X.prefix[1] && !X.prefix[2] && !X.prefix[3] &&
         X.opcode[0] == 0x0f && X.opcode[1] == 0x01 && !X.opcode[2] &&
         !X.opcode[3] && !X.rex && X.addr_size == 8 && X.modrm == 0xf8 &&
         !X.sib && !X.disp && X.encoding.modrm_offset == 2 &&
         !X.encoding.disp_offset && !X.encoding.disp_size &&
         !X.encoding.imm_offset && !X.encoding.imm_size && !X.op_count &&
         !X.avx_sae && X.avx_rm == X86_AVX_RM_INVALID &&
         X.avx_cc == X86_AVX_CC_INVALID && !X.eflags;
}

// Intel SDM Vol. 2, MOVS/REP, LDMXCSR, SWAPGS and CPUID: these forms introduce
// no undefined output bits. This does not certify nonfaulting execution, the
// MXCSR writable mask, a repeated transfer's count/direction, or opaque-state
// preservation. Their existing semantic owners retain those separate
// obligations.
inline bool form(const cs_insn *Insn, Arch Target) {
  if (Insn->id == X86_INS_CPUID)
    return cpuQueryForm(Insn, Target);
  if (Insn->id == X86_INS_SWAPGS)
    return swapgsForm(Insn, Target);
  const auto &X = Insn->detail->x86;
  if (Target != Arch::X64 || !Insn->size || Insn->size > 15 || X.avx_sae ||
      X.avx_rm != X86_AVX_RM_INVALID || X.avx_cc != X86_AVX_CC_INVALID ||
      X.encoding.imm_size || X.encoding.imm_offset)
    return false;
  if (Insn->id == X86_INS_MOVSQ) {
    // Keep the first admission exactly F3 48 A5: 64-bit count and pointers,
    // no source segment, address-size override or redundant prefix.
    if (Insn->size != 3 || Insn->bytes[0] != 0xf3 || Insn->bytes[1] != 0x48 ||
        Insn->bytes[2] != 0xa5 || X.prefix[0] != 0xf3 || X.prefix[1] ||
        X.prefix[2] || X.prefix[3] || X.opcode[0] != 0xa5 || X.opcode[1] ||
        X.opcode[2] || X.opcode[3] || X.rex != 0x48 || X.addr_size != 8 ||
        X.modrm || X.sib || X.disp || X.encoding.modrm_offset ||
        X.encoding.disp_size || X.encoding.disp_offset || X.op_count != 2)
      return false;
    for (unsigned I = 0; I != 2; ++I) {
      const auto &Operand = X.operands[I];
      if (Operand.type != X86_OP_MEM || Operand.size != 8 ||
          Operand.mem.segment != X86_REG_INVALID ||
          Operand.mem.base != (I ? X86_REG_RSI : X86_REG_RDI) ||
          Operand.mem.index != X86_REG_INVALID || Operand.mem.scale != 1 ||
          Operand.mem.disp || Operand.avx_bcast != X86_AVX_BCAST_INVALID ||
          Operand.avx_zero_opmask)
        return false;
    }
    return true;
  }
  if (Insn->id != X86_INS_LDMXCSR || X.op_count != 1)
    return false;

  size_t Pos = 0;
  uint8_t AddressPrefix = 0, Rex = 0;
  if (Insn->bytes[Pos] == 0x67) {
    AddressPrefix = 0x67;
    ++Pos;
  }
  // REX.B/X extend address registers. W/R are outside this closed form.
  if (Pos < Insn->size && Insn->bytes[Pos] >= 0x40 && Insn->bytes[Pos] <= 0x43)
    Rex = Insn->bytes[Pos++];
  if (Pos + 3 > Insn->size || Insn->bytes[Pos] != 0x0f ||
      Insn->bytes[Pos + 1] != 0xae)
    return false;
  const uint8_t ModRM = Insn->bytes[Pos + 2];
  const unsigned AddressSize = AddressPrefix ? 4 : 8;
  const auto &Operand = X.operands[0];
  if ((ModRM >> 6) == 3 || ((ModRM >> 3) & 7) != 2 || X.modrm != ModRM ||
      X.encoding.modrm_offset != Pos + 2 || X.opcode[0] != 0x0f ||
      X.opcode[1] != 0xae || X.opcode[2] || X.opcode[3] || X.rex != Rex ||
      X.prefix[0] || X.prefix[1] || X.prefix[2] ||
      X.prefix[3] != AddressPrefix || X.addr_size != AddressSize ||
      Operand.avx_bcast != X86_AVX_BCAST_INVALID || Operand.avx_zero_opmask)
    return false;
  return legacyaudit::memoryOperand(Insn, Operand, 4, AddressSize, true, Rex,
                                    Pos + 3);
}

} // namespace neverd::nativestateaudit

#endif
