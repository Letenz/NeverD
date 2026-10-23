//===- AArch64LiftCPA.cpp - Checked pointer arithmetic ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Scalar CPA reads runtime checking controls. Preserve an instruction-level
/// operation instead of replacing it with wrapping integer arithmetic.
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/AArch64CPA.h"
#include "neverd/support/BinaryEncoding.h"

namespace neverd {

bool liftCPA(AArch64Lifter &L, AArch64Lifter::LiftState &S, const cs_insn *Insn,
             const cs_aarch64 &A) {
  const auto reject = [&]() {
    throw UnliftedInstruction(S.Addr, Insn->mnemonic, Insn->op_str);
  };
  Intrinsic Id;
  switch (Insn->id) {
#define NEVERD_A64_CPA(IdValue, Insn, ...)                                     \
  case AARCH64_INS_##Insn:                                                     \
    Id = Intrinsic::IdValue;                                                   \
    break;
#include "neverd/ir/intrinsics/AArch64CPA.def"
  case AARCH64_INS_MADPT:
  case AARCH64_INS_MLAPT:
    // SVE forms require runtime-vector-width state.
    reject();
    return false;
  default:
    return false;
  }
  const auto Operation = *aarch64CPAOperation(Id);
  if ((readLE<uint32_t>(Insn->bytes) & Operation.Mask) != Operation.Encoding ||
      A.op_count != Operation.Sources + 1)
    reject();
  for (unsigned I = 0; I < A.op_count; ++I) {
    const auto &Operand = A.operands[I];
    if (Operand.type != AARCH64_OP_REG ||
        mapCapstoneReg(static_cast<aarch64_reg>(Operand.reg)).Size !=
            kAArch64CPAPointerBytes ||
        Operand.ext != AARCH64_EXT_INVALID || Operand.vector_index >= 0 ||
        (Operand.shift.type != AARCH64_SFT_INVALID &&
         (Operation.Sources != 2 || I != 2 ||
          Operand.shift.type != AARCH64_SFT_LSL ||
          Operand.shift.value > kAArch64CPAMaxShift)))
      reject();
  }
  const NdVar Dst = L.operandWrite(A.operands[0]);
  const NdVar First = L.operandRead(S, A.operands[1]);
  const NdVar Second = L.operandRead(S, A.operands[2]);
  if (Operation.Sources == 2)
    S.emitIntrinsic(Id, Dst, {First, Second});
  else
    S.emitIntrinsic(Id, Dst, {First, Second, L.operandRead(S, A.operands[3])});
  return true;
}

} // namespace neverd
