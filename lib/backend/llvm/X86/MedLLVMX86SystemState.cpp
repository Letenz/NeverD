//===- MedLLVMX86SystemState.cpp - Bounded system instruction lowering ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep system effects, conditional memory and actual flag completion in one
/// native instruction. Unsupported environments still fault architecturally.
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/X86SystemStateAsm.h"
#include "neverd/ir/med/IntrinsicShapes.h"

#include "llvm/IR/InlineAsm.h"
#include "llvm/Support/ErrorHandling.h"

#include <vector>

namespace neverd {

llvm::Value *MedLLVMEmitter::emitX86SystemState(const MedOp &Op, Intrinsic Id,
                                                llvm::IRBuilder<> &Builder) {
  const auto Shape = x86SystemStateMedShape(Op, TargetArch);
  if (!x86SystemStateShapeIsValid(Id, Shape))
    llvm::report_fatal_error("invalid x86 system instruction operand contract");
  const auto C = x86SystemStateContract(Id);
  const auto P = x86SystemStateAsm(
      Id, TargetArch, x86SystemStateWidth(Id, Shape), Op.MemoryAddressSpace);
  if (!P)
    llvm::report_fatal_error("unsupported x86 system assembly contract");
  std::vector<llvm::Value *> Arguments;
  std::vector<llvm::Type *> Types;
  for (size_t I = 0; I < P->InputBytes.size(); ++I) {
    auto *T = sizeToType(P->InputBytes[I]);
    llvm::Value *V;
    if (isX86SystemStateAddressArgument(Id, I)) {
      const auto Space = I == C.MemoryArgument ? Op.MemoryAddressSpace
                                               : NdMemoryAddressSpace::Default;
      const uint16_t AccessBytes =
          x86SystemStateMemoryBytes(Id, x86SystemStateWidth(Id, Shape));
      V = resolveAtomicMemoryPtr(Op.Inputs[I + 1], AccessBytes,
                                 sizeToType(AccessBytes), Builder, Space);
      if (Space == NdMemoryAddressSpace::Default)
        T = V->getType();
    } else
      V = getVar(Op.Inputs[I + 1], Builder);
    if (V->getType() != T)
      V = V->getType()->isPointerTy() ? Builder.CreatePtrToInt(V, T)
                                      : Builder.CreateZExtOrTrunc(V, T);
    Arguments.push_back(V);
    Types.push_back(T);
  }
  llvm::Type *Return = Builder.getVoidTy();
  if (P->OutputBytes.size() == 1)
    Return = sizeToType(P->OutputBytes[0]);
  else if (P->OutputBytes.size() == 2)
    Return = llvm::StructType::get(
        *Ctx, {sizeToType(P->OutputBytes[0]), Builder.getInt8Ty()});
  auto *Signature = llvm::FunctionType::get(Return, Types, false);
  auto *Asm = llvm::InlineAsm::get(Signature, P->Text, P->Constraints, true);
  auto *Call = Builder.CreateCall(Asm, Arguments);
  if (P->OutputBytes.empty())
    return nullptr;
  if (P->OutputBytes.size() == 1)
    return Call;
  auto *Value = Builder.CreateExtractValue(Call, 0);
  auto *Flag = Builder.CreateExtractValue(Call, 1);
  if (C.Kind == X86SystemStateKind::AccessRights)
    Value = Builder.CreateSelect(Builder.CreateICmpNE(Flag, Builder.getInt8(0)),
                                 Value,
                                 llvm::ConstantInt::get(Value->getType(), 0));
  auto *Wide = sizeToType(Op.Output.Size);
  return Builder.CreateOr(
      Builder.CreateZExt(Value, Wide),
      Builder.CreateShl(Builder.CreateZExt(Flag, Wide), 8 * P->OutputBytes[0]));
}

} // namespace neverd
