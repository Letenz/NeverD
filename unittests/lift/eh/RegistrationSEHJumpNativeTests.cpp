//===- RegistrationSEHJumpNativeTests.cpp - Nonlocal jump edits -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationSEHJumpNativeTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/X86RegistrationSEHJump.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"

namespace neverd::registration_test {

void checkSEHJumpEdits(const llvm::Function &Parent,
                       const ExceptionFunction &Source,
                       const BinaryImage &Image) {
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
  using x86_registration::SEHJumpRole;
  for (unsigned Mutation = 0; Mutation != 26; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Module = llvm::CloneModule(*Parent.getParent());
    auto *Function = Module->getFunction(Parent.getName());
    llvm::CallBase *Capture = nullptr, *Resume = nullptr;
    llvm::AllocaInst *Snapshot = nullptr;
    llvm::StoreInst *State = nullptr;
    for (auto &Block : *Function)
      for (auto &I : Block) {
        if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&I))
          if (const auto *Callee = Call->getCalledFunction()) {
            if (Callee->getName() == "_setjmp3")
              Capture = Call;
            if (Callee->getName() == "_longjmpex")
              Resume = Call;
          }
        if (auto *MD =
                I.getMetadata(windows_eh_md::RegistrationJumpAttachment)) {
          ASSERT_EQ(MD->getNumOperands(), 3u);
          const auto *Kind =
              llvm::mdconst::dyn_extract<llvm::ConstantInt>(MD->getOperand(2));
          ASSERT_TRUE(Kind);
          if (Kind->getZExtValue() == unsigned(SEHJumpRole::RegisterSnapshot))
            Snapshot = llvm::dyn_cast<llvm::AllocaInst>(&I);
          if (Kind->getZExtValue() == unsigned(SEHJumpRole::StateRestore))
            State = llvm::dyn_cast<llvm::StoreInst>(&I);
        }
      }
    ASSERT_TRUE(Capture && Resume && Snapshot && State);
    auto *Buffer = llvm::cast<llvm::AllocaInst>(Capture->getArgOperand(0));
    llvm::StoreInst *Save = nullptr, *Restore = nullptr;
    for (auto *User : Snapshot->users()) {
      if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(User))
        Save = Store;
      if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(User))
        Restore = llvm::dyn_cast<llvm::StoreInst>(*Load->user_begin());
    }
    ASSERT_TRUE(Save && Restore);
    llvm::IRBuilder<> B(Capture);
    if (Mutation == 0)
      Capture->setArgOperand(0, llvm::ConstantPointerNull::get(B.getPtrTy()));
    if (Mutation == 1)
      Resume->setArgOperand(0, llvm::ConstantPointerNull::get(B.getPtrTy()));
    if (Mutation == 2)
      Capture->setArgOperand(1, B.getInt32(2));
    if (Mutation == 3) {
      Capture->removeFnAttr(llvm::Attribute::ReturnsTwice);
      Capture->getCalledFunction()->removeFnAttr(llvm::Attribute::ReturnsTwice);
    }
    if (Mutation == 4)
      B.CreateStore(B.getInt32(0), Buffer)->setVolatile(true);
    if (Mutation == 5)
      B.CreateLoad(B.getInt32Ty(), Buffer)->setVolatile(true);
    if (Mutation == 6)
      Buffer->setMetadata(windows_eh_md::RegistrationJumpAttachment, nullptr);
    if (Mutation == 7)
      State->setOperand(0, B.getInt32(-1));
    if (Mutation == 8)
      State->setMetadata(windows_eh_md::RegistrationJumpAttachment, nullptr);
    if (Mutation == 9)
      Save->setOperand(
          0, llvm::Constant::getNullValue(Save->getValueOperand()->getType()));
    if (Mutation == 10)
      Restore->setOperand(0, llvm::Constant::getNullValue(
                                 Restore->getValueOperand()->getType()));
    if (Mutation == 11)
      Restore->setVolatile(false);
    if (Mutation == 12)
      Capture->setMetadata(windows_eh_md::RegistrationOperationAttachment,
                           nullptr);
    if (Mutation == 13)
      rewrite_source::setOriginalVA(*Capture->getCalledFunction(), 0x401000);
    if (Mutation == 14)
      Resume->setArgOperand(1, B.getInt32(0));
    if (Mutation == 15) {
      auto *Value = llvm::cast<llvm::LoadInst>(Resume->getArgOperand(1));
      auto *Address =
          llvm::cast<llvm::GetElementPtrInst>(Value->getPointerOperand());
      auto *Offset = llvm::cast<llvm::ConstantInt>(Address->getOperand(1));
      Address->setOperand(1, B.getInt32(Offset->getZExtValue() + 4));
    }
    if (Mutation == 16)
      Resume->addFnAttr(llvm::Attribute::NoCallback);
    if (Mutation == 17 || Mutation == 18) {
      auto *Unwind = Module->getFunction("_seh_longjmp_unwind");
      ASSERT_TRUE(Unwind);
      if (Mutation == 17)
        Unwind->setCallingConv(llvm::CallingConv::C);
      else
        rewrite_source::setOriginalVA(*Unwind, 0x401000);
    }
    if (Mutation >= 19) {
      auto *Unwind = Module->getFunction(x86_registration::SEHJumpUnwindName);
      auto *Runtime =
          Module->getFunction(x86_registration::SEHSourceJumpUnwindName);
      ASSERT_TRUE(Unwind && Runtime);
      for (auto &I : Unwind->getEntryBlock()) {
        if (auto *Copy = llvm::dyn_cast<llvm::MemCpyInst>(&I);
            Copy && Mutation == 19)
          Copy->setLength(B.getInt64(60));
        if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(&I)) {
          const bool Registration =
              GEP->getPointerOperand() == Unwind->getArg(0);
          if ((Mutation == 20 && Registration) ||
              (Mutation == 21 && !Registration))
            GEP->setOperand(1, B.getInt32(20));
        }
        if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I);
            Store && Mutation == 22)
          Store->setOperand(1, Unwind->getArg(0));
        if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&I);
            Call && Call->getCalledFunction() == Runtime) {
          if (Mutation == 24)
            Call->setArgOperand(0, Unwind->getArg(0));
          if (Mutation == 25)
            Call->addFnAttr(llvm::Attribute::NoCallback);
        }
      }
      if (Mutation == 23)
        rewrite_source::setOriginalVA(*Runtime, 0x401000);
    }
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    auto Error = validateCOFFRegistrationIR(*Function, Source, Image);
    ASSERT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
#endif
}

} // namespace neverd::registration_test
