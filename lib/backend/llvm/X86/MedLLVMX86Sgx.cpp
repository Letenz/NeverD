//===- MedLLVMX86Sgx.cpp - SGX register and flag completion --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep enclave memory and fault ordering inside the native instruction.
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/X86SgxAsm.h"
#include "neverd/ir/med/IntrinsicShapes.h"

#include "llvm/IR/InlineAsm.h"
#include "llvm/Support/ErrorHandling.h"

#include <vector>

namespace neverd {

llvm::Value *MedLLVMEmitter::emitX86Sgx(const MedOp &Op, Intrinsic Id,
                                        llvm::IRBuilder<> &B) {
  if (!x86SgxShapeIsValid(Id, x86SgxMedShape(Op, TargetArch)))
    llvm::report_fatal_error("invalid x86 SGX completion contract");
  const auto P = x86SgxAsm(Id, TargetArch);
  auto *Word = sizeToType(P->WordBytes);
  std::vector<llvm::Value *> Arguments;
  for (unsigned I = 1; I < Op.NumInputs; ++I) {
    auto *V = getVar(Op.Inputs[I], B);
    Arguments.push_back(V->getType()->isPointerTy()
                            ? B.CreatePtrToInt(V, Word)
                            : B.CreateZExtOrTrunc(V, Word));
  }
  auto *Result =
      llvm::StructType::get(*Ctx, {Word, Word, sizeToType(X86SgxFlagBytes)});
  auto *Signature = llvm::FunctionType::get(
      Result, std::vector<llvm::Type *>(Arguments.size(), Word), false);
  auto *Asm = llvm::InlineAsm::get(Signature, P->Text, P->Constraints, true);
  auto *Call = B.CreateCall(Asm, Arguments);
  PendingIntrinsicCount = X86SgxAuxiliaryOutputs;
  for (unsigned I = 0; I < X86SgxAuxiliaryOutputs; ++I)
    PendingIntrinsicOutputs[I] = B.CreateExtractValue(Call, I + 1);
  return B.CreateExtractValue(Call, 0);
}

} // namespace neverd
