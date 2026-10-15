//===- RegistrationSEHExceptionNativeTests.cpp - SEH runtime edits --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationSEHExceptionNativeTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/ir/RegistrationState.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"

namespace neverd::registration_test {

void checkSEHExceptionEdits(const llvm::Function &Parent,
                            const ExceptionFunction &Source,
                            const BinaryImage &Image,
                            const RegistrationStateAnalysis &States) {
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
  using Kind = RegistrationSEHExceptionAccess::Kind;
  for (unsigned Mutation = 0; Mutation != 12; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Module = llvm::CloneModule(*Parent.getParent());
    auto *Function = Module->getFunction(Parent.getName());
    llvm::LoadInst *Pointers = nullptr, *Record = nullptr, *Scalar = nullptr;
    Kind Field = Kind::ExceptionCode;
    for (auto &F : *Module)
      for (auto &Block : F)
        for (auto &I : Block) {
          auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I);
          const auto *MD =
              I.getMetadata(windows_eh_md::RegistrationOperationAttachment);
          if (!Load || !MD || MD->getNumOperands() != 5)
            continue;
          const auto *Address =
              llvm::mdconst::dyn_extract<llvm::ConstantInt>(MD->getOperand(1));
          const auto *Seq =
              llvm::mdconst::dyn_extract<llvm::ConstantInt>(MD->getOperand(2));
          if (!Address || !Seq)
            continue;
          for (const auto &Access : States.SEHExceptionAccesses) {
            if (Access.Address != Address->getZExtValue() ||
                uint32_t(Access.OpSeq) != Seq->getZExtValue())
              continue;
            if (Access.Field == Kind::PointersSlot)
              Pointers = Load;
            else if (Access.Field == Kind::RecordPointer)
              Record = Load;
            else {
              Scalar = Load;
              Field = Access.Field;
            }
          }
        }
    ASSERT_TRUE(Pointers && Record && Scalar);
    ASSERT_EQ(Pointers->getFunction(), Scalar->getFunction());
    llvm::IRBuilder<> B(Scalar);
    if (Mutation == 0)
      Scalar->setOperand(
          0, B.CreateGEP(B.getInt8Ty(), Scalar->getPointerOperand(),
                         B.getInt32(Field == Kind::ExceptionCode ? 4 : -4)));
    if (Mutation == 1)
      Scalar->setOperand(0, llvm::ConstantPointerNull::get(B.getPtrTy()));
    if (Mutation == 2) {
      auto *Wrong = B.CreateLoad(B.getInt16Ty(), Scalar->getPointerOperand());
      Wrong->copyMetadata(*Scalar);
      Wrong->setVolatile(true);
      Wrong->setAlignment(llvm::Align(1));
      Scalar->replaceAllUsesWith(B.CreateZExt(Wrong, B.getInt32Ty()));
      Scalar->eraseFromParent();
    }
    if (Mutation == 3)
      B.CreateLoad(B.getInt32Ty(), Scalar->getPointerOperand())
          ->setVolatile(true);
    if (Mutation == 4)
      B.CreateStore(B.getInt32(0), Scalar->getPointerOperand())
          ->setVolatile(true);
    if (Mutation == 5) {
      B.SetInsertPoint(Pointers);
      B.CreateStore(B.getInt32(0), Pointers->getPointerOperand())
          ->setVolatile(true);
    }
    if (Mutation == 6) {
      bool Changed = false;
      for (auto &Block : *Scalar->getFunction())
        if (auto *Return =
                llvm::dyn_cast<llvm::ReturnInst>(Block.getTerminator())) {
          Return->setOperand(0, Record);
          Changed = true;
        }
      ASSERT_TRUE(Changed);
    }
    if (Mutation == 7)
      Pointers->setOperand(0, llvm::ConstantPointerNull::get(B.getPtrTy()));
    if (Mutation == 8) {
      auto *Global = new llvm::GlobalVariable(
          *Module, B.getInt32Ty(), false, llvm::GlobalValue::ExternalLinkage,
          nullptr, "__nd_data_403000");
      B.CreateStore(Record, Global)->setVolatile(true);
    }
    if (Mutation == 9)
      Scalar->setMetadata(windows_eh_md::RegistrationOperationAttachment,
                          nullptr);
    if (Mutation == 10) {
      auto *Extra = B.CreateLoad(B.getInt32Ty(), Scalar->getPointerOperand());
      Extra->copyMetadata(*Scalar);
      Extra->setVolatile(true);
      Extra->setAlignment(llvm::Align(1));
    }
    if (Mutation == 11) {
      B.SetInsertPoint(Pointers);
      auto *Extra = B.CreateLoad(B.getInt32Ty(), Pointers->getPointerOperand());
      Extra->setVolatile(true);
      for (auto &Block : *Scalar->getFunction())
        if (auto *Return =
                llvm::dyn_cast<llvm::ReturnInst>(Block.getTerminator()))
          Return->setOperand(0, Extra);
    }
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    auto Error = validateCOFFRegistrationIR(*Function, Source, Image);
    ASSERT_TRUE(bool(Error));
    const auto Message = llvm::toString(std::move(Error));
    EXPECT_TRUE(Message.find("SEH exception") != std::string::npos ||
                Message.find("synthetic frame") != std::string::npos)
        << Message;
  }
#endif
}

} // namespace neverd::registration_test
