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
#include "X86LiftBMI2.h"
#include "X86LiftBMILogic.h"
#include "X86LiftBitCount.h"
#include "X86LiftDetail.h"

#include "neverd/lift/X86/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

namespace {

struct ApxAdxEncoding {
  bool Present = false;
  bool Valid = false;
  unsigned Source = 0;
  unsigned Source2 = 1;
};

ApxAdxEncoding decodeApxAdx(const cs_insn *Insn, const cs_x86 &X86) {
  ApxAdxEncoding Result;
  Result.Present = apxvalidation::isPresent(Insn, X86);
  if (!Result.Present)
    return Result;
  apxvalidation::Header H;
  if (!apxvalidation::decodeHeader(Insn, X86, H))
    return Result;
  const unsigned PP = Insn->id == X86_INS_ADCX ? 1 : 2;
  const bool NDD = H.P2 & 0x10;
  const unsigned Width = H.P1 & 0x80 ? 8 : 4;
  const uint64_t Flags = Insn->id == X86_INS_ADCX
                             ? X86_EFLAGS_TEST_CF | X86_EFLAGS_MODIFY_CF
                             : X86_EFLAGS_TEST_OF | X86_EFLAGS_MODIFY_OF;
  if ((H.P0 & 7) != 4 || H.Opcode != 0x66 || (H.P1 & 3) != PP ||
      (H.P2 & 0xe7) != 0 || (!H.Memory && (H.P1 & 4) == 0) ||
      (!NDD && apxvalidation::vvvvv(H) != 0) ||
      !apxvalidation::implicitDetail(Insn, X86, Flags, {X86_REG_EFLAGS},
                                     {X86_REG_EFLAGS}))
    return Result;
  if (NDD) {
    if (X86.op_count != 3 ||
        !apxvalidation::registerOperand(
            X86.operands[0], apxvalidation::vvvvv(H), Width, CS_AC_WRITE) ||
        !apxvalidation::registerOperand(
            X86.operands[1], apxvalidation::modrmReg(H), Width, CS_AC_READ) ||
        !apxvalidation::validateRM(Insn, X86, H, X86.operands[2], Width,
                                   CS_AC_READ))
      return Result;
    Result.Source = 1;
    Result.Source2 = 2;
  } else if (X86.op_count != 2 ||
             !apxvalidation::registerOperand(X86.operands[0],
                                             apxvalidation::modrmReg(H), Width,
                                             CS_AC_READ | CS_AC_WRITE) ||
             !apxvalidation::validateRM(Insn, X86, H, X86.operands[1], Width,
                                        CS_AC_READ)) {
    return Result;
  }
  Result.Valid = true;
  return Result;
}

} // namespace

bool liftExtBMI(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                const cs_x86 &X86) {
  if (bmi2::isInstruction(Insn->id))
    return bmi2::lift(L, S, Insn);
  if (bmilogic::isInstruction(Insn->id))
    return bmilogic::lift(L, S, Insn);
  if (bitcount::isInstruction(Insn->id))
    return bitcount::lift(L, S, Insn);
  if (Insn->id == X86_INS_BEXTR)
    return bextr::lift(L, S, Insn);
  unsigned InsnId = Insn->id;
  if (InsnId != X86_INS_ADCX && InsnId != X86_INS_ADOX)
    return false;
  const ApxAdxEncoding Apx = decodeApxAdx(Insn, X86);
  if (Apx.Present && (!Apx.Valid || L.targetArch() != Arch::X64))
    return false;
  switch (InsnId) {

  // ========================================================================
  // ADX: ADCX / ADOX — multi-precision addition (reads/writes single flag).
  // ========================================================================
  case X86_INS_ADCX:
  case X86_INS_ADOX: {
    if (!Apx.Present && X86.op_count < 2)
      break;
    const unsigned DstIndex = 0;
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
