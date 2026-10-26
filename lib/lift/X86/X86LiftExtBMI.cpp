//===- X86LiftExtBMI.cpp - x86/x64 BMI/BMI2/ADX lifter --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bit-manipulation extensions: TZCNT/LZCNT/POPCNT, the BLS*
/// lowest-set-bit family, ANDN, BEXTR, BZHI, MULX, PDEP/PEXT,
/// RORX, the flag-preserving shifts SARX/SHLX/SHRX, and the
/// ADX carry chains ADCX/ADOX.
///
//===----------------------------------------------------------------------===//

#include "X86LiftAPXValidation.h"
#include "X86LiftBEXTR.h"
#include "X86LiftBMILogic.h"
#include "X86LiftBitCount.h"
#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86/X86Lifter.h"

#include <limits>

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

namespace {

constexpr unsigned NoOperand = std::numeric_limits<unsigned>::max();

int apxGprIndex(x86_reg Reg, unsigned Width) {
  static const x86_reg Low16[] = {X86_REG_AX, X86_REG_CX, X86_REG_DX,
                                  X86_REG_BX, X86_REG_SP, X86_REG_BP,
                                  X86_REG_SI, X86_REG_DI};
  static const x86_reg Low32[] = {X86_REG_EAX, X86_REG_ECX, X86_REG_EDX,
                                  X86_REG_EBX, X86_REG_ESP, X86_REG_EBP,
                                  X86_REG_ESI, X86_REG_EDI};
  static const x86_reg Low64[] = {X86_REG_RAX, X86_REG_RCX, X86_REG_RDX,
                                  X86_REG_RBX, X86_REG_RSP, X86_REG_RBP,
                                  X86_REG_RSI, X86_REG_RDI};
  if (Width != 2 && Width != 4 && Width != 8)
    return -1;
  const x86_reg *Low = Width == 2 ? Low16 : Width == 4 ? Low32 : Low64;
  for (unsigned I = 0; I != 8; ++I)
    if (Reg == Low[I])
      return static_cast<int>(I);
  if (Width == 2) {
    if (Reg >= X86_REG_R8W && Reg <= X86_REG_R15W)
      return 8 + static_cast<int>(Reg - X86_REG_R8W);
    if (Reg >= X86_REG_R16W && Reg <= X86_REG_R31W)
      return 16 + static_cast<int>(Reg - X86_REG_R16W);
  } else if (Width == 4) {
    if (Reg >= X86_REG_R8D && Reg <= X86_REG_R15D)
      return 8 + static_cast<int>(Reg - X86_REG_R8D);
    if (Reg >= X86_REG_R16D && Reg <= X86_REG_R31D)
      return 16 + static_cast<int>(Reg - X86_REG_R16D);
  } else {
    if (Reg >= X86_REG_R8 && Reg <= X86_REG_R15)
      return 8 + static_cast<int>(Reg - X86_REG_R8);
    if (Reg >= X86_REG_R16 && Reg <= X86_REG_R31)
      return 16 + static_cast<int>(Reg - X86_REG_R16);
  }
  return -1;
}

x86_reg apxSegmentRegister(uint8_t Prefix) {
  switch (Prefix) {
  case 0x26:
    return X86_REG_ES;
  case 0x2e:
    return X86_REG_CS;
  case 0x36:
    return X86_REG_SS;
  case 0x3e:
    return X86_REG_DS;
  case 0x64:
    return X86_REG_FS;
  case 0x65:
    return X86_REG_GS;
  default:
    return X86_REG_INVALID;
  }
}

bool isApxSegmentPrefix(uint8_t Prefix) {
  return apxSegmentRegister(Prefix) != X86_REG_INVALID;
}

bool isPotentialApxPrefix(uint8_t Prefix) {
  return isApxSegmentPrefix(Prefix) || Prefix == 0x67 || Prefix == 0x66 ||
         Prefix == 0xf0 || Prefix == 0xf2 || Prefix == 0xf3 ||
         (Prefix >= 0x40 && Prefix <= 0x4f);
}

struct ApxHeader {
  size_t EvexOffset = 0;
  uint8_t SegmentPrefix = 0;
  bool Address32 = false;
  uint8_t P0 = 0;
  uint8_t P1 = 0;
  uint8_t P2 = 0;
  uint8_t Opcode = 0;
  uint8_t ModRM = 0;
  bool Memory = false;
};

struct ApxBmiEncoding {
  bool Present = false;
  bool Valid = false;
  bool NF = false;
  bool NDD = false;
  unsigned Width = 0;
  unsigned Destination = NoOperand;
  unsigned Destination2 = NoOperand;
  unsigned Source = NoOperand;
  unsigned Source2 = NoOperand;
};

bool decodeApxHeader(const cs_insn *Insn, const cs_x86 &X86,
                     ApxBmiEncoding &Result, ApxHeader &Header,
                     unsigned TrailingImmediateBytes = 0) {
  if (!Insn || Insn->size == 0 || Insn->size > 15)
    return false;
  apxvalidation::EvexPrefixes Prefixes;
  if (!apxvalidation::scanEvexPrefixes(Insn, Prefixes))
    return false;
  const size_t Offset = Prefixes.EvexOffset;
  Header.SegmentPrefix = Prefixes.SegmentPrefix;
  Header.Address32 = Prefixes.Address32;

  Result.Present = true;
  Header.EvexOffset = Offset;
  if (Offset + 6 > Insn->size)
    return false;
  Header.P0 = Insn->bytes[Offset + 1];
  Header.P1 = Insn->bytes[Offset + 2];
  Header.P2 = Insn->bytes[Offset + 3];
  Header.Opcode = Insn->bytes[Offset + 4];
  Header.ModRM = Insn->bytes[Offset + 5];
  Header.Memory = (Header.ModRM & 0xc0) != 0xc0;
  const unsigned ExpectedImmediateOffset =
      TrailingImmediateBytes == 0 ? 0 : Insn->size - TrailingImmediateBytes;
  if (TrailingImmediateBytes > Insn->size ||
      X86.encoding.modrm_offset != Offset + 5 || X86.modrm != Header.ModRM ||
      X86.encoding.imm_offset != ExpectedImmediateOffset ||
      X86.encoding.imm_size != TrailingImmediateBytes ||
      X86.addr_size != (Header.Address32 ? 4 : 8) || X86.prefix[0] != 0 ||
      X86.prefix[1] != Header.SegmentPrefix || X86.prefix[2] != 0 ||
      X86.prefix[3] != (Header.Address32 ? 0x67 : 0))
    return false;
  for (unsigned I = 0; I != 4; ++I)
    if (X86.opcode[I] != Insn->bytes[Offset + I])
      return false;
  return true;
}

bool apxRegisterOperand(const cs_x86_op &Operand, unsigned Register,
                        unsigned Width, uint8_t Access) {
  return Operand.type == X86_OP_REG && Operand.size == Width &&
         Operand.access == Access &&
         apxGprIndex(static_cast<x86_reg>(Operand.reg), Width) ==
             static_cast<int>(Register);
}

bool apxAddressRegister(x86_reg Register, unsigned Number, unsigned Width) {
  return apxGprIndex(Register, Width) == static_cast<int>(Number);
}

bool validateApxMemory(const cs_insn *Insn, const cs_x86 &X86,
                       const ApxHeader &Header, const cs_x86_op &Operand,
                       unsigned Width, unsigned TrailingImmediateBytes = 0) {
  if (!Header.Memory || Operand.type != X86_OP_MEM || Operand.size != Width ||
      Operand.access != CS_AC_READ ||
      Operand.mem.segment != apxSegmentRegister(Header.SegmentPrefix))
    return false;

  const uint8_t Mod = Header.ModRM >> 6;
  const uint8_t RM = Header.ModRM & 7;
  const unsigned AddressWidth = Header.Address32 ? 4 : 8;
  const unsigned BaseExtension =
      ((~Header.P0 & 0x20) >> 2) | ((Header.P0 & 0x08) << 1);
  const unsigned IndexExtension =
      ((~Header.P0 & 0x40) >> 3) | ((~Header.P1 & 0x04) << 2);
  size_t Cursor = Header.EvexOffset + 6;
  uint8_t DisplacementSize = 0;
  x86_reg ExpectedSpecialBase = X86_REG_INVALID;
  int ExpectedBase = -1;
  int ExpectedIndex = -1;
  int ExpectedScale = 1;
  bool HasSIB = false;
  uint8_t SIB = 0;

  if (RM == 4) {
    if (Cursor >= Insn->size)
      return false;
    HasSIB = true;
    SIB = Insn->bytes[Cursor++];
    ExpectedScale = 1U << (SIB >> 6);
    const unsigned IndexLow = (SIB >> 3) & 7;
    const unsigned BaseLow = SIB & 7;
    if (IndexLow != 4 || IndexExtension != 0)
      ExpectedIndex = static_cast<int>(IndexLow + IndexExtension);
    if (Mod == 0 && BaseLow == 5)
      DisplacementSize = 4;
    else
      ExpectedBase = static_cast<int>(BaseLow + BaseExtension);
  } else if (Mod == 0 && RM == 5) {
    ExpectedSpecialBase = Header.Address32 ? X86_REG_EIP : X86_REG_RIP;
    DisplacementSize = 4;
  } else {
    ExpectedBase = static_cast<int>(RM + BaseExtension);
  }
  if (Mod == 1)
    DisplacementSize = 1;
  else if (Mod == 2)
    DisplacementSize = 4;

  int64_t Displacement = 0;
  const size_t DisplacementOffset = Cursor;
  if (DisplacementSize == 1) {
    if (Cursor >= Insn->size)
      return false;
    Displacement = static_cast<int8_t>(Insn->bytes[Cursor++]);
  } else if (DisplacementSize == 4) {
    if (Insn->size - Cursor < 4)
      return false;
    uint32_t Raw = 0;
    for (unsigned I = 0; I != 4; ++I)
      Raw |= static_cast<uint32_t>(Insn->bytes[Cursor + I]) << (I * 8);
    Displacement = static_cast<int32_t>(Raw);
    Cursor += 4;
  }
  if (TrailingImmediateBytes > Insn->size ||
      Cursor != Insn->size - TrailingImmediateBytes ||
      X86.encoding.disp_size != DisplacementSize ||
      X86.encoding.disp_offset !=
          (DisplacementSize != 0 ? DisplacementOffset : 0) ||
      X86.disp != Displacement || X86.sib != (HasSIB ? SIB : 0) ||
      Operand.mem.disp != Displacement || Operand.mem.scale != ExpectedScale)
    return false;

  if (HasSIB) {
    if (X86.sib_base != Operand.mem.base ||
        X86.sib_index != Operand.mem.index || X86.sib_scale != ExpectedScale)
      return false;
  } else if (X86.sib_base != X86_REG_INVALID ||
             X86.sib_index != X86_REG_INVALID || X86.sib_scale != 0) {
    return false;
  }

  if (ExpectedSpecialBase != X86_REG_INVALID) {
    if (Operand.mem.base != ExpectedSpecialBase)
      return false;
  } else if (ExpectedBase < 0) {
    if (Operand.mem.base != X86_REG_INVALID)
      return false;
  } else if (!apxAddressRegister(static_cast<x86_reg>(Operand.mem.base),
                                 static_cast<unsigned>(ExpectedBase),
                                 AddressWidth)) {
    return false;
  }
  if (ExpectedIndex < 0)
    return Operand.mem.index == X86_REG_INVALID;
  return apxAddressRegister(static_cast<x86_reg>(Operand.mem.index),
                            static_cast<unsigned>(ExpectedIndex), AddressWidth);
}

bool validateApxRM(const cs_insn *Insn, const cs_x86 &X86,
                   const ApxHeader &Header, const cs_x86_op &Operand,
                   unsigned Width, unsigned TrailingImmediateBytes = 0) {
  if (Header.Memory)
    return validateApxMemory(Insn, X86, Header, Operand, Width,
                             TrailingImmediateBytes);
  if (Insn->size != Header.EvexOffset + 6 + TrailingImmediateBytes ||
      X86.encoding.disp_offset != 0 || X86.encoding.disp_size != 0 ||
      X86.disp != 0 || X86.sib != 0 || X86.sib_base != X86_REG_INVALID ||
      X86.sib_index != X86_REG_INVALID || X86.sib_scale != 0)
    return false;
  const unsigned Register = ((~Header.P0 & 0x20) >> 2) |
                            ((Header.P0 & 0x08) << 1) | (Header.ModRM & 7);
  return apxRegisterOperand(Operand, Register, Width, CS_AC_READ);
}

bool validateApxDetail(const cs_insn *Insn, const cs_x86 &X86,
                       uint64_t ExpectedFlags,
                       x86_reg ExpectedRead = X86_REG_INVALID,
                       x86_reg ExpectedWrite = X86_REG_INVALID) {
  if (!Insn || !Insn->detail || X86.eflags != ExpectedFlags)
    return false;
  const cs_detail &Detail = *Insn->detail;
  const uint8_t ExpectedReadCount = ExpectedRead == X86_REG_INVALID ? 0 : 1;
  const uint8_t ExpectedWriteCount = ExpectedWrite == X86_REG_INVALID ? 0 : 1;
  return Detail.regs_read_count == ExpectedReadCount &&
         Detail.regs_write_count == ExpectedWriteCount &&
         (ExpectedReadCount == 0 || Detail.regs_read[0] == ExpectedRead) &&
         (ExpectedWriteCount == 0 || Detail.regs_write[0] == ExpectedWrite);
}

ApxBmiEncoding decodeApxBmiEncoding(const cs_insn *Insn, const cs_x86 &X86) {
  ApxBmiEncoding Result;
  switch (Insn ? Insn->id : X86_INS_INVALID) {
  case X86_INS_MULX:
  case X86_INS_PDEP:
  case X86_INS_PEXT:
  case X86_INS_RORX:
  case X86_INS_SARX:
  case X86_INS_SHLX:
  case X86_INS_SHRX:
  case X86_INS_ADCX:
  case X86_INS_ADOX:
    break;
  default:
    return Result;
  }

  // Remember that this is an APX byte shape before validating prefixes, so a
  // repeated segment/address override cannot fall back to the legacy lifter.
  size_t Marker = 0;
  while (Marker < Insn->size && isPotentialApxPrefix(Insn->bytes[Marker]))
    ++Marker;
  Result.Present = X86.opcode[0] == 0x62 ||
                   (Marker < Insn->size && Insn->bytes[Marker] == 0x62);

  ApxHeader H;
  const bool HasTrailingImmediate = Insn->id == X86_INS_RORX;
  if (!decodeApxHeader(Insn, X86, Result, H, HasTrailingImmediate ? 1 : 0))
    return Result;
  const unsigned Id = Insn->id;
  const unsigned ModRMReg =
      ((~H.P0 & 0x80) >> 4) | (~H.P0 & 0x10) | ((H.ModRM >> 3) & 7);
  const unsigned Vvvvv = ((~H.P2 & 0x08) << 1) | ((~H.P1 & 0x78) >> 3);
  unsigned Width = (H.P1 & 0x80) ? 8 : 4;
  Result.Width = Width;

  if (Id == X86_INS_RORX) {
    const cs_x86_op &Immediate = X86.operands[2];
    if ((H.P0 & 7) != 3 || H.Opcode != 0xf0 || (H.P1 & 0x7b) != 0x7b ||
        (!H.Memory && (H.P1 & 4) == 0) || H.P2 != 0x08 || X86.op_count != 3 ||
        !validateApxDetail(Insn, X86, 0) ||
        !apxRegisterOperand(X86.operands[0], ModRMReg, Width, CS_AC_WRITE) ||
        !validateApxRM(Insn, X86, H, X86.operands[1], Width, 1) ||
        Immediate.type != X86_OP_IMM || Immediate.size != 1 ||
        Immediate.access != CS_AC_READ ||
        static_cast<uint8_t>(Immediate.imm) != Insn->bytes[Insn->size - 1])
      return Result;
    Result.Destination = 0;
    Result.Source = 1;
    Result.Source2 = 2;
    Result.Valid = true;
    return Result;
  }

  if (Id == X86_INS_SARX || Id == X86_INS_SHLX || Id == X86_INS_SHRX) {
    const unsigned PP = H.P1 & 3;
    const unsigned ExpectedPP = Id == X86_INS_SHLX   ? 1
                                : Id == X86_INS_SARX ? 2
                                                     : 3;
    if ((H.P0 & 7) != 2 || H.Opcode != 0xf7 || (!H.Memory && (H.P1 & 4) == 0) ||
        PP != ExpectedPP || (H.P2 & 0xf7) != 0 || X86.op_count != 3 ||
        !validateApxDetail(Insn, X86, 0) ||
        !apxRegisterOperand(X86.operands[0], ModRMReg, Width, CS_AC_WRITE) ||
        !validateApxRM(Insn, X86, H, X86.operands[1], Width) ||
        !apxRegisterOperand(X86.operands[2], Vvvvv, Width, CS_AC_READ))
      return Result;
    Result.Destination = 0;
    Result.Source = 1;
    Result.Source2 = 2;
    Result.Valid = true;
    return Result;
  }

  if (Id == X86_INS_PDEP || Id == X86_INS_PEXT) {
    const unsigned ExpectedPP = Id == X86_INS_PDEP ? 3 : 2;
    if ((H.P0 & 7) != 2 || H.Opcode != 0xf5 || (H.P1 & 3) != ExpectedPP ||
        (H.P2 & 0xf7) != 0 || (!H.Memory && (H.P1 & 4) == 0) ||
        X86.op_count != 3 || !validateApxDetail(Insn, X86, 0) ||
        !apxRegisterOperand(X86.operands[0], ModRMReg, Width, CS_AC_WRITE) ||
        !apxRegisterOperand(X86.operands[1], Vvvvv, Width, CS_AC_READ) ||
        !validateApxRM(Insn, X86, H, X86.operands[2], Width))
      return Result;
    Result.Destination = 0;
    Result.Source = 1;
    Result.Source2 = 2;
    Result.Valid = true;
    return Result;
  }

  if (Id == X86_INS_MULX) {
    if ((H.P0 & 7) != 2 || H.Opcode != 0xf6 || (H.P1 & 3) != 3 ||
        (!H.Memory && (H.P1 & 4) == 0) || (H.P2 & 0xf7) != 0 ||
        X86.op_count != 3 ||
        !validateApxDetail(Insn, X86, 0,
                           Width == 4 ? X86_REG_EDX : X86_REG_RDX) ||
        !apxRegisterOperand(X86.operands[0], ModRMReg, Width, CS_AC_WRITE) ||
        !apxRegisterOperand(X86.operands[1], Vvvvv, Width, CS_AC_WRITE) ||
        !validateApxRM(Insn, X86, H, X86.operands[2], Width))
      return Result;
    Result.Destination = 0;
    Result.Destination2 = 1;
    Result.Source = 2;
    Result.Valid = true;
    return Result;
  }

  if (Id == X86_INS_ADCX || Id == X86_INS_ADOX) {
    const unsigned ExpectedPP = Id == X86_INS_ADCX ? 1 : 2;
    const bool NDD = (H.P2 & 0x10) != 0;
    const uint64_t ExpectedFlags =
        Id == X86_INS_ADCX ? X86_EFLAGS_TEST_CF | X86_EFLAGS_MODIFY_CF
                           : X86_EFLAGS_TEST_OF | X86_EFLAGS_MODIFY_OF;
    if ((H.P0 & 7) != 4 || H.Opcode != 0x66 || (H.P1 & 3) != ExpectedPP ||
        (H.P2 & 0xe7) != 0 || (!H.Memory && (H.P1 & 4) == 0) ||
        (!NDD && Vvvvv != 0) ||
        !validateApxDetail(Insn, X86, ExpectedFlags, X86_REG_EFLAGS,
                           X86_REG_EFLAGS))
      return Result;
    if (NDD) {
      if (X86.op_count != 3 ||
          !apxRegisterOperand(X86.operands[0], Vvvvv, Width, CS_AC_WRITE) ||
          !apxRegisterOperand(X86.operands[1], ModRMReg, Width, CS_AC_READ) ||
          !validateApxRM(Insn, X86, H, X86.operands[2], Width))
        return Result;
      Result.Destination = 0;
      Result.Source = 1;
      Result.Source2 = 2;
    } else {
      if (X86.op_count != 2 ||
          !apxRegisterOperand(X86.operands[0], ModRMReg, Width,
                              CS_AC_READ | CS_AC_WRITE) ||
          !validateApxRM(Insn, X86, H, X86.operands[1], Width))
        return Result;
      Result.Destination = 0;
      Result.Source = 0;
      Result.Source2 = 1;
    }
    Result.NDD = NDD;
    Result.Valid = true;
    return Result;
  }
  return Result;
}

void emitMulx64(X86Lifter::LiftState &S, NdVar Left, NdVar Right, NdVar &Low,
                NdVar &High) {
  constexpr uint64_t Lo32Mask = UINT64_C(0xffffffff);
  NdVar A0 = S.makeTemp(8), A1 = S.makeTemp(8);
  NdVar B0 = S.makeTemp(8), B1 = S.makeTemp(8);
  S.emit(NdOp::INT_AND, A0, {Left, NdVar::scalar(Lo32Mask, 8)});
  S.emit(NdOp::INT_RIGHT, A1, {Left, NdVar::scalar(32, 8)});
  S.emit(NdOp::INT_AND, B0, {Right, NdVar::scalar(Lo32Mask, 8)});
  S.emit(NdOp::INT_RIGHT, B1, {Right, NdVar::scalar(32, 8)});

  NdVar P0 = S.makeTemp(8), Cross10 = S.makeTemp(8);
  NdVar Cross01 = S.makeTemp(8), P3 = S.makeTemp(8);
  S.emit(NdOp::INT_MULT, P0, {A0, B0});
  S.emit(NdOp::INT_MULT, Cross10, {A1, B0});
  S.emit(NdOp::INT_MULT, Cross01, {A0, B1});
  S.emit(NdOp::INT_MULT, P3, {A1, B1});

  NdVar P0High = S.makeTemp(8), T = S.makeTemp(8);
  S.emit(NdOp::INT_RIGHT, P0High, {P0, NdVar::scalar(32, 8)});
  S.emit(NdOp::INT_ADD, T, {Cross10, P0High});
  NdVar TLow = S.makeTemp(8), THigh = S.makeTemp(8);
  S.emit(NdOp::INT_AND, TLow, {T, NdVar::scalar(Lo32Mask, 8)});
  S.emit(NdOp::INT_RIGHT, THigh, {T, NdVar::scalar(32, 8)});
  NdVar Middle = S.makeTemp(8);
  S.emit(NdOp::INT_ADD, Middle, {TLow, Cross01});
  NdVar MiddleHigh = S.makeTemp(8);
  S.emit(NdOp::INT_RIGHT, MiddleHigh, {Middle, NdVar::scalar(32, 8)});
  NdVar HighBase = S.makeTemp(8);
  S.emit(NdOp::INT_ADD, HighBase, {P3, THigh});
  High = S.makeTemp(8);
  S.emit(NdOp::INT_ADD, High, {HighBase, MiddleHigh});

  NdVar MiddleLow = S.makeTemp(8), LowUpper = S.makeTemp(8);
  NdVar P0Low = S.makeTemp(8);
  S.emit(NdOp::INT_AND, MiddleLow, {Middle, NdVar::scalar(Lo32Mask, 8)});
  S.emit(NdOp::INT_LEFT, LowUpper, {MiddleLow, NdVar::scalar(32, 8)});
  S.emit(NdOp::INT_AND, P0Low, {P0, NdVar::scalar(Lo32Mask, 8)});
  Low = S.makeTemp(8);
  S.emit(NdOp::INT_OR, Low, {LowUpper, P0Low});
}

} // namespace

bool liftExtBMI(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                const cs_x86 &X86) {
  if (bmilogic::isInstruction(Insn->id))
    return bmilogic::lift(L, S, Insn);
  if (bitcount::isInstruction(Insn->id))
    return bitcount::lift(L, S, Insn);
  if (Insn->id == X86_INS_BEXTR)
    return bextr::lift(L, S, Insn);
  unsigned InsnId = Insn->id;
  const ApxBmiEncoding Apx = decodeApxBmiEncoding(Insn, X86);
  if (Apx.Present && !Apx.Valid)
    return false;
  switch (InsnId) {

  // BMI2 value operations preserve every arithmetic flag.
  case X86_INS_MULX: {
    if (!Apx.Present && X86.op_count < 3)
      break;
    const unsigned DstHiIndex = Apx.Present ? Apx.Destination : 0;
    const unsigned DstLoIndex = Apx.Present ? Apx.Destination2 : 1;
    const unsigned SrcIndex = Apx.Present ? Apx.Source : 2;
    NdVar Src = L.operandRead(S, X86.operands[SrcIndex]);
    uint16_t Sz = Src.Size;
    NdVar Rdx = NdVar::reg(x86reg::RDX, Sz);
    NdVar Low, High;
    if (Sz == 8) {
      emitMulx64(S, Rdx, Src, Low, High);
    } else {
      NdVar ExtA = S.makeTemp(8);
      NdVar ExtB = S.makeTemp(8);
      S.emit(NdOp::INT_ZEXT, ExtA, {Rdx});
      S.emit(NdOp::INT_ZEXT, ExtB, {Src});
      NdVar Full = S.makeTemp(8);
      S.emit(NdOp::INT_MULT, Full, {ExtA, ExtB});
      Low = S.makeTemp(4);
      High = S.makeTemp(4);
      S.emit(NdOp::SUBBYTES, Low, {Full, NdVar::scalar(0, 4)});
      S.emit(NdOp::SUBBYTES, High, {Full, NdVar::scalar(4, 4)});
    }
    NdVar DstLo = L.operandWrite(X86.operands[DstLoIndex]);
    NdVar DstHi = L.operandWrite(X86.operands[DstHiIndex]);
    S.emit(NdOp::COPY, DstLo, {Low});
    S.emit(NdOp::COPY, DstHi, {High});
    break;
  }

  case X86_INS_PDEP: {
    if (!Apx.Present && X86.op_count < 3)
      break;
    const unsigned DstIndex = Apx.Present ? Apx.Destination : 0;
    const unsigned SrcIndex = Apx.Present ? Apx.Source : 1;
    const unsigned MaskIndex = Apx.Present ? Apx.Source2 : 2;
    NdVar Src = L.operandRead(S, X86.operands[SrcIndex]);
    NdVar Mask = L.operandRead(S, X86.operands[MaskIndex]);
    NdVar Dst = L.operandWrite(X86.operands[DstIndex]);
    S.emitIntrinsic(Intrinsic::Pdep, Dst, {Src, Mask});
    break;
  }
  case X86_INS_PEXT: {
    if (!Apx.Present && X86.op_count < 3)
      break;
    const unsigned DstIndex = Apx.Present ? Apx.Destination : 0;
    const unsigned SrcIndex = Apx.Present ? Apx.Source : 1;
    const unsigned MaskIndex = Apx.Present ? Apx.Source2 : 2;
    NdVar Src = L.operandRead(S, X86.operands[SrcIndex]);
    NdVar Mask = L.operandRead(S, X86.operands[MaskIndex]);
    NdVar Dst = L.operandWrite(X86.operands[DstIndex]);
    S.emitIntrinsic(Intrinsic::Pext, Dst, {Src, Mask});
    break;
  }

  case X86_INS_RORX: {
    if (!Apx.Present && X86.op_count < 3)
      break;
    const unsigned DstIndex = Apx.Present ? Apx.Destination : 0;
    const unsigned SrcIndex = Apx.Present ? Apx.Source : 1;
    const unsigned CountIndex = Apx.Present ? Apx.Source2 : 2;
    NdVar Dst = L.operandWrite(X86.operands[DstIndex]);
    NdVar Src = L.operandRead(S, X86.operands[SrcIndex]);
    NdVar CntRaw = L.operandRead(S, X86.operands[CountIndex]);
    if (CntRaw.isConst())
      CntRaw.Provenance = ConstantAddressProvenance::Scalar;
    uint16_t Sz = Dst.Size;
    uint16_t Bits = Sz * 8;
    uint64_t RorxMask = (Bits == 64) ? 0x3F : 0x1F;
    NdVar Cnt = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, Cnt, {CntRaw, NdVar::scalar(RorxMask, Sz)});
    NdVar Shr = S.makeTemp(Sz);
    NdVar Comp = S.makeTemp(Sz);
    NdVar Shl = S.makeTemp(Sz);
    S.emit(NdOp::INT_RIGHT, Shr, {Src, Cnt});
    S.emit(NdOp::INT_SUB, Comp, {NdVar::scalar(Bits, Sz), Cnt});
    S.emit(NdOp::INT_AND, Comp, {Comp, NdVar::scalar(Bits - 1, Sz)});
    S.emit(NdOp::INT_LEFT, Shl, {Src, Comp});
    S.emit(NdOp::INT_OR, Dst, {Shr, Shl});
    break;
  }

  case X86_INS_SARX:
  case X86_INS_SHLX:
  case X86_INS_SHRX: {
    if (!Apx.Present && X86.op_count < 3)
      break;
    const unsigned DstIndex = Apx.Present ? Apx.Destination : 0;
    const unsigned SrcIndex = Apx.Present ? Apx.Source : 1;
    const unsigned CountIndex = Apx.Present ? Apx.Source2 : 2;
    NdVar Src = L.operandRead(S, X86.operands[SrcIndex]);
    NdVar CntRaw = L.operandRead(S, X86.operands[CountIndex]);
    NdVar Dst = L.operandWrite(X86.operands[DstIndex]);
    if (CntRaw.isConst())
      CntRaw.Provenance = ConstantAddressProvenance::Scalar;
    uint16_t Sz = Dst.Size;
    uint16_t Bits = Sz * 8;
    uint64_t VexMask = (Bits == 64) ? 0x3F : 0x1F;
    NdVar Cnt = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, Cnt, {CntRaw, NdVar::scalar(VexMask, Sz)});
    NdOp Opc;
    switch (InsnId) {
    case X86_INS_SHLX:
      Opc = NdOp::INT_LEFT;
      break;
    case X86_INS_SHRX:
      Opc = NdOp::INT_RIGHT;
      break;
    default:
      Opc = NdOp::INT_ASHR;
    }
    S.emit(Opc, Dst, {Src, Cnt});
    break;
  }

  // ========================================================================
  // ADX: ADCX / ADOX — multi-precision addition (reads/writes single flag).
  // ========================================================================
  case X86_INS_ADCX:
  case X86_INS_ADOX: {
    if (!Apx.Present && X86.op_count < 2)
      break;
    const unsigned DstIndex = Apx.Present ? Apx.Destination : 0;
    const unsigned FirstIndex = Apx.Present ? Apx.Source : 0;
    const unsigned SecondIndex = Apx.Present ? Apx.Source2 : 1;
    NdVar First = L.operandRead(S, X86.operands[FirstIndex]);
    NdVar Second = L.operandRead(S, X86.operands[SecondIndex]);
    NdVar Dst = L.operandWrite(X86.operands[DstIndex]);
    const uint64_t Flag = InsnId == X86_INS_ADCX ? x86reg::CF : x86reg::OF;
    NdVar CarryExt = S.makeTemp(First.Size);
    S.emit(NdOp::INT_ZEXT, CarryExt, {NdVar::reg(Flag, 1)});
    NdVar C1 = S.makeTemp(1);
    S.emit(NdOp::INT_CARRY, C1, {Second, CarryExt});
    NdVar Adjusted = S.makeTemp(First.Size);
    S.emit(NdOp::INT_ADD, Adjusted, {Second, CarryExt});
    NdVar C2 = S.makeTemp(1);
    S.emit(NdOp::INT_CARRY, C2, {First, Adjusted});
    NdVar Result = S.makeTemp(First.Size);
    S.emit(NdOp::INT_ADD, Result, {First, Adjusted});
    NdVar CarryOut = S.makeTemp(1);
    S.emit(NdOp::BOOL_OR, CarryOut, {C1, C2});
    S.emit(NdOp::COPY, Dst, {Result});
    S.emit(NdOp::COPY, NdVar::reg(Flag, 1), {CarryOut});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
