//===- RegistrationCxxContinuationTestUtils.cpp - Catch resumes ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationCxxContinuationTestUtils.h"

#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/Cloning.h"

namespace neverd::registration_test {

void checkCxxContinuationEdits(const llvm::Function &Parent,
                               const ExceptionFunction &Source,
                               const BinaryImage &Image) {
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Edited = llvm::CloneModule(*Parent.getParent());
    auto *Function = Edited->getFunction(Parent.getName());
    llvm::CatchReturnInst *Return = nullptr;
    for (auto &Block : *Function)
      if (auto *Candidate =
              llvm::dyn_cast<llvm::CatchReturnInst>(Block.getTerminator())) {
        ASSERT_EQ(Return, nullptr);
        Return = Candidate;
      }
    ASSERT_NE(Return, nullptr);
    auto *Store = llvm::dyn_cast<llvm::StoreInst>(Return->getPrevNode());
    ASSERT_NE(Store, nullptr);
    ASSERT_TRUE(Store->isVolatile());
    auto *Value = llvm::dyn_cast<llvm::PtrToIntInst>(Store->getValueOperand());
    ASSERT_NE(Value, nullptr);
    auto *Saved =
        llvm::dyn_cast<llvm::GetElementPtrInst>(Value->getPointerOperand());
    auto *Slot =
        llvm::dyn_cast<llvm::GetElementPtrInst>(Store->getPointerOperand());
    ASSERT_TRUE(Saved && Slot);
    switch (Mutation) {
    case 0:
      Store->eraseFromParent();
      break;
    case 1:
      Store->setOperand(0, llvm::ConstantInt::get(Value->getType(), 7));
      break;
    case 2:
    case 3: {
      auto *GEP = Mutation == 2 ? Slot : Saved;
      auto *Index = llvm::cast<llvm::ConstantInt>(GEP->getOperand(1));
      GEP->setOperand(1, llvm::ConstantInt::get(Index->getType(),
                                                Index->getZExtValue() + 4));
      break;
    }
    case 4:
      Store->setVolatile(false);
      break;
    case 5:
      Store->setAtomic(llvm::AtomicOrdering::Monotonic);
      break;
    case 6:
      Slot->getPrevNode()->moveBefore(Return->getIterator());
      break;
    case 7:
      Store->clone()->insertBefore(Return->getIterator());
      break;
    }
    auto Changed = validateCOFFRegistrationCxxIR(*Function, Source, Image);
    EXPECT_TRUE(bool(Changed));
    if (Changed)
      llvm::consumeError(std::move(Changed));
  }
}

} // namespace neverd::registration_test
