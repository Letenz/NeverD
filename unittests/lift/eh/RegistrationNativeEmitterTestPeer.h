//===- RegistrationNativeEmitterTestPeer.h - SEH test access ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared native registration test access to exact emitted source occurrences.

#ifndef NEVERD_UNITTESTS_LIFT_EH_REGISTRATIONNATIVEEMITTERTESTPEER_H
#define NEVERD_UNITTESTS_LIFT_EH_REGISTRATIONNATIVEEMITTERTESTPEER_H

#include "neverd/backend/llvm/LanguageEHMetadata.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

namespace neverd {
class MedLLVMEmitterTestPeer {
public:
  static void prepare(MedLLVMEmitter &E, llvm::Module &M, llvm::Function &F,
                      const MedFunc &Source, llvm::AllocaInst &Frame) {
    E.Ctx = &M.getContext();
    E.Mod = &M;
    E.TargetArch = Arch::X86;
    E.TargetFormat = BinaryFormat::COFF;
    E.CurFunc = &F;
    E.CurMedFunc = &Source;
    E.FrameAlloca = &Frame;
    E.FrameEntrySPOffset = 64;
  }
  static void chain(MedLLVMEmitter &E, llvm::Instruction &I, va_t Address,
                    int Seq) {
    E.RegistrationChainIR.emplace(std::make_pair(Address, Seq), &I);
  }
  static void call(MedLLVMEmitter &E, llvm::CallInst &I, va_t Address) {
    E.CallSiteAddrs.emplace(&I, Address);
    I.setMetadata(language_eh_md::InternalSourceCallAttachment,
                  llvm::MDNode::get(
                      I.getContext(),
                      {llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                          llvm::Type::getInt64Ty(I.getContext()), Address))}));
  }
  static void forgetCall(MedLLVMEmitter &E, llvm::CallInst &I) {
    E.CallSiteAddrs.erase(&I);
  }
  static void memory(MedLLVMEmitter &E, llvm::Instruction &I, va_t Address,
                     int Seq) {
    E.RegistrationMemoryIR.emplace(std::make_pair(Address, Seq), &I);
  }
  static bool lower(MedLLVMEmitter &E, const MedFunc &Source, llvm::Function &F,
                    const std::map<int, llvm::BasicBlock *> &Blocks) {
    return E.emitNativeX86RegistrationSEH(Source, F, Blocks, {});
  }
};
} // namespace neverd

#endif // NEVERD_UNITTESTS_LIFT_EH_REGISTRATIONNATIVEEMITTERTESTPEER_H
