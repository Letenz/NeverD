//===- MedLLVMRegistrationSEHStateDispatch.cpp - State selection ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Selects a physical callback state from the source store's complete result.

#include "MedLLVMRegistrationSEHState.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicsX86.h"
#include "llvm/MC/BinaryRewrite.h"

namespace neverd::x86_registration {

void emitSEHCallbackStateDispatch(
    llvm::StoreInst &Store, llvm::ArrayRef<SEHCallbackStateTarget> Targets,
    llvm::Function &Parent, llvm::Value &Frame) {
#ifdef LLVM_NEVERD_X86_REGISTRATION_SEH_STATE_WRITES
  assert(!Targets.empty() && "a checked state store must have an outcome");
  auto Emit = [&](llvm::IRBuilder<> &B, const SEHCallbackStateTarget &Target) {
    B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                     Parent.getParent(), llvm::Intrinsic::x86_seh_setstate),
                 {&Parent, &Frame, B.getInt32(Target.Level),
                  B.getInt64(Target.Digest[0]), B.getInt64(Target.Digest[1]),
                  B.getInt64(Target.Digest[2]), B.getInt64(Target.Digest[3]),
                  B.getInt64(Target.ID)});
  };
  if (Targets.size() == 1) {
    llvm::IRBuilder<> B(Store.getNextNode());
    Emit(B, Targets.front());
    return;
  }
  auto *Block = Store.getParent();
  auto *Resume = Block->splitBasicBlock(Store.getNextNode()->getIterator(),
                                        "registration.state.resume");
  Block->getTerminator()->eraseFromParent();
  llvm::IRBuilder<> B(Block);
  auto *Current = B.CreateLoad(B.getInt32Ty(), Store.getPointerOperand(),
                               "registration.state.current");
  Current->setAlignment(llvm::Align(1));
  Current->setVolatile(true);
  std::vector<llvm::BasicBlock *> Cases;
  for (const auto &Target : Targets) {
    auto *Case = llvm::BasicBlock::Create(Parent.getContext(),
                                          "registration.state.write",
                                          Store.getFunction(), Resume);
    llvm::IRBuilder<> CaseBuilder(Case);
    Emit(CaseBuilder, Target);
    CaseBuilder.CreateBr(Resume);
    Cases.push_back(Case);
  }
  // The source transitions prove the finite domain. The default represents its
  // last member; every other complete result has an explicit selector.
  auto *Select = B.CreateSwitch(Current, Cases.back(), Targets.size() - 1);
  for (size_t I = 0; I + 1 != Targets.size(); ++I)
    Select->addCase(B.getInt32(Targets[I].Level), Cases[I]);
#else
  llvm_unreachable("state dispatch requires the LLVM write protocol");
#endif
}

} // namespace neverd::x86_registration
