//===- RegistrationSEHDynamicScopeTests.cpp - Dynamic state mutations ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationSEHDynamicScopeTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"

namespace neverd::registration_test {

void checkDynamicSEHScopeEdits(const llvm::Function &Parent,
                               const ExceptionFunction &Source,
                               const BinaryImage &Image) {
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Module = llvm::CloneModule(*Parent.getParent());
    auto *Function = Module->getFunction(Parent.getName());
    llvm::LoadInst *State = nullptr;
    llvm::SwitchInst *Switch = nullptr;
    llvm::SelectInst *Restore = nullptr;
    llvm::StoreInst *SourceWrite = nullptr;
    for (auto &Block : *Function)
      for (auto &I : Block) {
        if (auto *Choice = llvm::dyn_cast<llvm::SwitchInst>(&I))
          if (auto *Read =
                  llvm::dyn_cast<llvm::LoadInst>(Choice->getCondition());
              Read && Read->getName().starts_with("registration.scope")) {
            State = Read;
            if (Read->getParent() == &Block)
              Switch = Choice;
          }
        const auto *MD =
            I.getMetadata(windows_eh_md::RegistrationOperationAttachment);
        if (auto *Write = llvm::dyn_cast<llvm::StoreInst>(&I)) {
          if (I.getMetadata(windows_eh_md::RegistrationJumpAttachment))
            Restore =
                llvm::dyn_cast<llvm::SelectInst>(Write->getValueOperand());
          if (MD && MD->getNumOperands() == 5) {
            const auto *Address = llvm::mdconst::dyn_extract<llvm::ConstantInt>(
                MD->getOperand(1));
            if (Address && llvm::any_of(Source.Registration->TryLevelStores,
                                        [&](const auto &S) {
                                          return S.StoreVA ==
                                                 Address->getZExtValue();
                                        }))
              SourceWrite = Write;
          }
        }
      }
    ASSERT_TRUE(State && Switch && Restore && SourceWrite);
    auto *Address =
        llvm::cast<llvm::GetElementPtrInst>(State->getPointerOperand());
    llvm::IRBuilder<> B(State);
    switch (Mutation) {
    case 0:
      Switch->setCondition(B.getInt32(0));
      break;
    case 1:
      State->setVolatile(false);
      break;
    case 2:
      Address->setOperand(1, B.getInt32(0));
      break;
    case 3:
      Switch->case_begin()->setSuccessor(Switch->getDefaultDest());
      break;
    case 4:
      B.CreateStore(B.getInt32(0), Address);
      break;
    case 5:
      B.CreateStore(B.getInt8(0),
                    B.CreateGEP(B.getInt8Ty(), Address, B.getInt32(1)));
      break;
    case 6:
      Restore->setFalseValue(Restore->getTrueValue());
      break;
    case 7:
      SourceWrite->setOperand(0, B.getInt32(42));
      break;
    }
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    auto Error = validateCOFFRegistrationIR(*Function, Source, Image);
    ASSERT_TRUE(bool(Error));
    const auto Message = llvm::toString(std::move(Error));
    EXPECT_TRUE(Message.find("scope") != std::string::npos ||
                Message.find("source try level") != std::string::npos)
        << Message;
  }
#endif
}

} // namespace neverd::registration_test
