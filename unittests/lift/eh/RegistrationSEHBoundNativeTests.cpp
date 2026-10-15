//===- RegistrationSEHBoundNativeTests.cpp - Preserved SEH call edits -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Reject mutated native argument contracts for preserved SEH helpers.

#include "RegistrationSEHJumpNativeTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"

namespace neverd::registration_test {

void checkSEHBoundEdits(const llvm::Function &Parent,
                        const ExceptionFunction &Source,
                        const BinaryImage &Image) {
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
  for (unsigned Mutation = 0; Mutation != 18; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Module = llvm::CloneModule(*Parent.getParent());
    auto *Function = Module->getFunction(Parent.getName());
    llvm::CallBase *Helper = nullptr, *Capture = nullptr;
    for (auto &Block : *Function)
      for (auto &I : Block)
        if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&I))
          if (const auto *Callee = Call->getCalledFunction()) {
            if (Callee->getName().starts_with("__nd_seh_bound_"))
              Helper = Call;
            if (Callee->getName() == "_setjmp3")
              Capture = Call;
          }
    ASSERT_TRUE(Helper && Capture);
    ASSERT_GE(Helper->arg_size(), 2u);
    const bool HasScalar = Helper->arg_size() == 3;
    if (Mutation >= 15 && !HasScalar)
      continue;
    llvm::IRBuilder<> B(Helper);
    auto *Buffer = Capture->getArgOperand(0);
    auto *Object = Helper->getArgOperand(1);
    const bool BorrowsBuffer =
        Helper->getArgOperand(0)->getType()->isPointerTy();
    if (Mutation == 0)
      Helper->setArgOperand(0, BorrowsBuffer ? Object : B.getInt32(1));
    if (Mutation == 1)
      Helper->setArgOperand(1, Buffer);
    if (Mutation == 2)
      Helper->setArgOperand(
          0, BorrowsBuffer ? B.CreateAlloca(B.getInt8Ty(), B.getInt32(64))
                           : static_cast<llvm::Value *>(B.getInt32(12)));
    if (Mutation == 3)
      Helper->setArgOperand(
          1, B.CreateInBoundsGEP(B.getInt8Ty(), Object, B.getInt32(4)));
    if (Mutation == 4)
      rewrite_source::setOriginalVA(*Helper->getCalledFunction(),
                                    Source.CodeRange.Begin);
    if (Mutation == 5)
      Helper->setCallingConv(llvm::CallingConv::X86_StdCall);
    if (Mutation == 6) {
      if (Helper->doesNotReturn())
        Helper->removeFnAttr(llvm::Attribute::NoReturn);
      else
        Helper->addFnAttr(llvm::Attribute::NoReturn);
    }
    if (Mutation == 7)
      Helper->addFnAttr(llvm::Attribute::NoUnwind);
    if (Mutation == 8)
      Helper->addFnAttr(llvm::Attribute::NoCallback);
    if (Mutation == 9)
      Helper->setMetadata(windows_eh_md::RegistrationOperationAttachment,
                          nullptr);
    if (Mutation == 10)
      Capture->setArgOperand(0, Object);
    if (Mutation == 11)
      B.CreateStore(B.getInt32(0), Buffer)->setVolatile(true);
    if (Mutation == 12) {
      if (auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(Helper))
        B.SetInsertPoint(&*Invoke->getNormalDest()->getFirstInsertionPt());
      else
        B.SetInsertPoint(Helper->getNextNode());
      auto *Observed = new llvm::GlobalVariable(
          *Module, B.getInt32Ty(), false, llvm::GlobalValue::InternalLinkage,
          B.getInt32(0), "observed.helper.result");
      B.CreateStore(Helper, Observed)->setVolatile(true);
    }
    if (Mutation == 13)
      Helper->addParamAttr(1, llvm::Attribute::getWithByValType(
                                  Module->getContext(), B.getInt32Ty()));
    if (Mutation == 14)
      Helper->getCalledFunction()->addParamAttr(1, llvm::Attribute::ReadOnly);
    if (Mutation >= 15) {
      auto *Scalar = llvm::dyn_cast<llvm::LoadInst>(Helper->getArgOperand(2));
      ASSERT_TRUE(Scalar);
      if (Mutation == 15)
        Scalar->setVolatile(false);
      if (Mutation == 16) {
        B.SetInsertPoint(Scalar);
        Scalar->setOperand(0, B.CreateInBoundsGEP(B.getInt8Ty(),
                                                  Scalar->getPointerOperand(),
                                                  B.getInt32(4)));
      }
      if (Mutation == 17) {
        B.SetInsertPoint(Scalar->getNextNode());
        B.CreateStore(B.getInt32(0), Scalar->getPointerOperand())
            ->setVolatile(true);
      }
    }
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    auto Error = validateCOFFRegistrationIR(*Function, Source, Image);
    ASSERT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
#endif
}

} // namespace neverd::registration_test
