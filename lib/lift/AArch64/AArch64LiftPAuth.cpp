//===- AArch64LiftPAuth.cpp - AArch64 pointer authentication ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Armv8.3 pointer signing, authentication, stripping and authenticated
/// control flow, including the implicit-register HINT encodings used by arm64e.
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/decode/Decoder.h"
#include "neverd/support/BinaryEncoding.h"

namespace neverd {

bool liftPAuth(AArch64Lifter &L, AArch64Lifter::LiftState &S,
               const cs_insn *Insn, const cs_aarch64 &A) {
  const auto reject = [&]() {
    throw UnliftedInstruction(S.Addr, Insn->mnemonic, Insn->op_str);
  };
  // Decode the fixed HINT words before the generic HINT/NOP handler. Both
  // Capstone's alias and non-alias presentations must have the same semantics.
  const uint32_t Word = readLE<uint32_t>(Insn->bytes);
  if ((Word & 0xfffff01fu) == 0xd503201fu) {
    Intrinsic Id;
    NdVar Pointer = NdVar::reg(a64reg::X30, 8);
    NdVar Modifier = NdVar::cst(0, 8);
    const unsigned Hint = (Word >> 5) & 0x7f;
    switch (Hint) {
    case 7:
      S.emitIntrinsic(Intrinsic::Xpaci, Pointer, {Pointer});
      return true;
    case 8:
    case 24:
    case 25:
      Id = Intrinsic::Pacia;
      break;
    case 10:
    case 26:
    case 27:
      Id = Intrinsic::Pacib;
      break;
    case 12:
    case 28:
    case 29:
      Id = Intrinsic::Autia;
      break;
    case 14:
    case 30:
    case 31:
      Id = Intrinsic::Autib;
      break;
    default:
      return false;
    }
    if (Hint < 16) {
      Pointer = NdVar::reg(a64reg::X17, 8);
      Modifier = NdVar::reg(a64reg::X16, 8);
    } else if (Hint & 1) {
      Modifier = NdVar::reg(a64reg::SP, 8);
    }
    S.emitIntrinsic(Id, Pointer, {Pointer, Modifier});
    return true;
  }

  Intrinsic Id;
  bool Zero = false;
  bool Strip = false;
  switch (Insn->id) {
  case AARCH64_INS_PACIZA:
    Zero = true;
    [[fallthrough]];
  case AARCH64_INS_PACIA:
    Id = Intrinsic::Pacia;
    break;
  case AARCH64_INS_PACIZB:
    Zero = true;
    [[fallthrough]];
  case AARCH64_INS_PACIB:
    Id = Intrinsic::Pacib;
    break;
  case AARCH64_INS_PACDZA:
    Zero = true;
    [[fallthrough]];
  case AARCH64_INS_PACDA:
    Id = Intrinsic::Pacda;
    break;
  case AARCH64_INS_PACDZB:
    Zero = true;
    [[fallthrough]];
  case AARCH64_INS_PACDB:
    Id = Intrinsic::Pacdb;
    break;
  case AARCH64_INS_AUTIZA:
    Zero = true;
    [[fallthrough]];
  case AARCH64_INS_AUTIA:
    Id = Intrinsic::Autia;
    break;
  case AARCH64_INS_AUTIZB:
    Zero = true;
    [[fallthrough]];
  case AARCH64_INS_AUTIB:
    Id = Intrinsic::Autib;
    break;
  case AARCH64_INS_AUTDZA:
    Zero = true;
    [[fallthrough]];
  case AARCH64_INS_AUTDA:
    Id = Intrinsic::Autda;
    break;
  case AARCH64_INS_AUTDZB:
    Zero = true;
    [[fallthrough]];
  case AARCH64_INS_AUTDB:
    Id = Intrinsic::Autdb;
    break;
  case AARCH64_INS_XPACI:
    Id = Intrinsic::Xpaci;
    Strip = true;
    break;
  case AARCH64_INS_XPACD:
    Id = Intrinsic::Xpacd;
    Strip = true;
    break;
  default: {
    bool Call = false, Return = false, KeyB = false;
    switch (Insn->id) {
    case AARCH64_INS_BLRABZ:
      Zero = true;
      [[fallthrough]];
    case AARCH64_INS_BLRAB:
      Call = true;
      KeyB = true;
      break;
    case AARCH64_INS_BLRAAZ:
      Zero = true;
      [[fallthrough]];
    case AARCH64_INS_BLRAA:
      Call = true;
      break;
    case AARCH64_INS_BRABZ:
      Zero = true;
      [[fallthrough]];
    case AARCH64_INS_BRAB:
      KeyB = true;
      break;
    case AARCH64_INS_BRAAZ:
      Zero = true;
      [[fallthrough]];
    case AARCH64_INS_BRAA:
      break;
    case AARCH64_INS_RETAB:
      KeyB = true;
      [[fallthrough]];
    case AARCH64_INS_RETAA:
      Return = true;
      break;
    // These need exception-level state or FEAT_PAuth_LR's second modifier.
    // They must never fall back to an ordinary ERET/RET or operand-less PAC.
    case AARCH64_INS_ERETAA:
    case AARCH64_INS_ERETAB:
    case AARCH64_INS_RETAASPPC:
    case AARCH64_INS_RETABSPPC:
    case AARCH64_INS_AUTIA171615:
    case AARCH64_INS_AUTIASPPC:
    case AARCH64_INS_AUTIB171615:
    case AARCH64_INS_AUTIBSPPC:
    case AARCH64_INS_PACIA171615:
    case AARCH64_INS_PACIASPPC:
    case AARCH64_INS_PACNBIASPPC:
    case AARCH64_INS_PACIB171615:
    case AARCH64_INS_PACIBSPPC:
    case AARCH64_INS_PACNBIBSPPC:
      reject();
      return false;
    default:
      return false;
    }
    if (!Return &&
        (A.op_count != (Zero ? 1 : 2) || A.operands[0].type != AARCH64_OP_REG ||
         (!Zero && A.operands[1].type != AARCH64_OP_REG)))
      reject();
    NdVar Pointer =
        Return ? NdVar::reg(a64reg::X30, 8) : L.operandRead(S, A.operands[0]);
    NdVar Modifier = Return ? NdVar::reg(a64reg::SP, 8)
                     : Zero ? NdVar::cst(0, 8)
                            : L.operandRead(S, A.operands[1]);
    NdVar Target = S.makeTemp(8);
    S.emitIntrinsic(Intrinsic::A64_AuthBranch, Target,
                    {Pointer, NdVar::cst(KeyB ? 1 : 0, 4), Modifier});
    // Capture/authenticate the old target and modifier before writing LR:
    // either source register of BLRAA/BLRAB can itself be x30.
    if (Call)
      S.emit(NdOp::COPY, NdVar::reg(a64reg::X30, 8),
             {NdVar::cst(S.Addr + 4, 8)});
    S.emit(Return ? NdOp::RETURN
           : Call ? NdOp::INDIR_CALL
                  : NdOp::INDIR_BR,
           Call ? NdVar::reg(a64reg::X0, 8) : NdVar{}, {Target});
    return true;
  }
  }
  if (A.op_count != (Zero || Strip ? 1 : 2) ||
      A.operands[0].type != AARCH64_OP_REG ||
      (!Zero && !Strip && A.operands[1].type != AARCH64_OP_REG))
    reject();
  NdVar Pointer = L.operandRead(S, A.operands[0]);
  NdVar Dst = L.operandWrite(A.operands[0]);
  if (Strip)
    S.emitIntrinsic(Id, Dst, {Pointer});
  else
    S.emitIntrinsic(
        Id, Dst,
        {Pointer, Zero ? NdVar::cst(0, 8) : L.operandRead(S, A.operands[1])});
  return true;
}

} // namespace neverd
