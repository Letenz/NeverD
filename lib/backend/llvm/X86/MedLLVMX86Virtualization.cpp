//===- MedLLVMX86Virtualization.cpp - VMX/SVM lowering -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Emit instruction-owned memory accesses and actual VMX completion flags.
/// Ordinary execution still requires the instruction's architectural privilege
/// and processor state; the emitter does not simulate a VMCS or assume success.
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/X86VirtualizationAsm.h"
#include "neverd/ir/med/IntrinsicShapes.h"

#include "llvm/IR/InlineAsm.h"
#include "llvm/Support/ErrorHandling.h"

#include <vector>

namespace neverd {

llvm::Value *MedLLVMEmitter::emitX86Virtualization(const MedOp &Op,
                                                   Intrinsic Id,
                                                   llvm::IRBuilder<> &Builder) {
  if (!x86VirtualizationShapeIsValid(Id,
                                     x86VirtualizationMedShape(Op, TargetArch)))
    llvm::report_fatal_error(
        llvm::Twine("x86 virtualization intrinsic has an invalid "
                    "operand/output contract: id=") +
        llvm::Twine(static_cast<unsigned>(Id)) +
        " arch=" + llvm::Twine(static_cast<unsigned>(TargetArch)) +
        " output=" + llvm::Twine(Op.Output.Size) +
        " inputs=" + llvm::Twine(static_cast<unsigned>(Op.NumInputs)));
  const auto C = x86VirtualizationContract(Id);
  const uint16_t Word = TargetArch == Arch::X64 ? 8 : 4;
  const auto P =
      x86VirtualizationAsm(Id, TargetArch, Op.MemoryAddressSpace,
                           C.PhysicalAddress ? Op.Inputs[1].Size : 0);
  if (!P)
    llvm::report_fatal_error(
        "unsupported x86 virtualization assembly contract");
  std::vector<llvm::Value *> Arguments;
  std::vector<llvm::Type *> Types;
  for (uint8_t I = 0; I < C.Arguments; ++I) {
    llvm::Value *Value = nullptr;
    if (I == C.MemoryArgument) {
      const uint16_t AccessBytes = C.MemoryBytes ? C.MemoryBytes : Word;
      Value = resolveAtomicMemoryPtr(Op.Inputs[I + 1], AccessBytes,
                                     sizeToType(AccessBytes), Builder,
                                     Op.MemoryAddressSpace);
    } else
      Value = getVar(Op.Inputs[I + 1], Builder);
    auto *Type = sizeToType(P->InputBytes[I]);
    const bool Address = I == C.MemoryArgument &&
                         Op.MemoryAddressSpace == NdMemoryAddressSpace::Default;
    // The pointer escape and memory clobber invalidate previous frame values.
    if (Address)
      Type = Value->getType();
    else if (Value->getType()->isPointerTy())
      Value = Builder.CreatePtrToInt(Value, Type);
    else if (Value->getType() != Type)
      Value = Builder.CreateZExtOrTrunc(Value, Type);
    Arguments.push_back(Value);
    Types.push_back(Type);
  }
  llvm::Type *Return = Builder.getVoidTy();
  if (C.Result == X86VirtualizationResult::Read)
    Return =
        llvm::StructType::get(*Ctx, {sizeToType(Word), Builder.getInt16Ty()});
  else if (C.Result == X86VirtualizationResult::Flags)
    Return = Builder.getInt16Ty();
  auto *Signature = llvm::FunctionType::get(Return, Types, false);
  auto *Asm = llvm::InlineAsm::get(Signature, P->Text, P->Constraints, true);
  auto *Call = Builder.CreateCall(Asm, Arguments);
  if (C.Result == X86VirtualizationResult::None)
    return nullptr;
  if (C.Result == X86VirtualizationResult::Flags)
    return Call;
  auto *Value = Builder.CreateExtractValue(Call, 0);
  auto *Flags = Builder.CreateExtractValue(Call, 1);
  // A failed VMREAD does not define a new architectural value. Only the
  // success arm can demand the physical output; the lifter merges the old
  // destination on failure without speculatively reading unknown old bits.
  Value =
      Builder.CreateSelect(Builder.CreateICmpEQ(Flags, Builder.getInt16(0)),
                           Value, llvm::ConstantInt::get(sizeToType(Word), 0));
  auto *Wide = sizeToType(2 * Word);
  return Builder.CreateOr(
      Builder.CreateZExt(Value, Wide),
      Builder.CreateShl(Builder.CreateZExt(Flags, Wide), 8 * Word));
}

} // namespace neverd
