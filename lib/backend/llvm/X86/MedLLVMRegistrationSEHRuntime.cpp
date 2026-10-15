//===- MedLLVMRegistrationSEHRuntime.cpp - Authenticated import calls ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Supply actual native arguments for checked PE32 runtime import calls.

#include "MedLLVMRegistrationSEHJump.h"

#include "neverd/backend/llvm/MedLLVMEmitter.h"

#include "llvm/IR/Module.h"

namespace neverd {

llvm::Function *
MedLLVMEmitter::getX86RegistrationSEHRuntimeTarget(const MedOp &Op) {
  if (TargetArch != Arch::X86 || TargetFormat != BinaryFormat::COFF ||
      !CurMedFunc || !CurMedFunc->RegistrationStates || !FrameAlloca ||
      Op.Opcode != NdOp::INDIR_CALL)
    return nullptr;
  const auto Calls = getX86RegistrationSEHBoundCalls(*CurMedFunc);
  if (!Calls)
    return nullptr;
  const auto Found = Calls->find(Op.Addr);
  if (Found == Calls->end() || Found->second.Operation != &Op)
    return nullptr;
  const auto &Source = *Found->second.Source;
  const auto Name = getX86RegistrationSEHBoundImport(Source);
  auto *Callee = Name.empty() ? nullptr : Mod->getFunction(Name);
  if (!Callee || !Callee->isDeclaration() ||
      Callee->getFunctionType() !=
          getX86RegistrationSEHBoundType(Source, *Ctx) ||
      !Callee->hasDLLImportStorageClass() ||
      Callee->getCallingConv() !=
          getX86RegistrationSEHBoundConvention(Source) ||
      !x86_registration::hasSEHBoundFrameArguments(Source, *FrameAlloca,
                                                   FrameEntrySPOffset))
    return nullptr;
  return Callee;
}

llvm::Function *MedLLVMEmitter::prepareX86RegistrationSEHRuntimeCall(
    const MedOp &Op, llvm::IRBuilder<> &Builder,
    std::vector<llvm::Value *> &Arguments) {
  auto *Callee = getX86RegistrationSEHRuntimeTarget(Op);
  if (!Callee)
    return nullptr;
  const auto Calls = getX86RegistrationSEHBoundCalls(*CurMedFunc);
  const auto &Source = *Calls->at(Op.Addr).Source;
  const auto Projected = x86_registration::projectSEHBoundArguments(
      Source, Builder, *FrameAlloca, FrameEntrySPOffset, {});
  Arguments.assign(Projected.begin(), Projected.end());
  return Callee;
}

} // namespace neverd
