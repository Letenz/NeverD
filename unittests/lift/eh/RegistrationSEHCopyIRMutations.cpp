//===- RegistrationSEHCopyIRMutations.cpp - Callback copy edits -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationSEHCopyIRMutations.h"

#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace neverd::registration_test {
void checkSEHCopyEdits(const llvm::Function &Parent,
                       const ExceptionFunction &Source,
                       const BinaryImage &Image) {
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
  std::vector<std::pair<std::string, size_t>> Sites;
  for (const auto &Function : *Parent.getParent()) {
    if (&Function == &Parent || Function.isDeclaration())
      continue;
    size_t Index = 0;
    for (const auto &Block : Function) {
      bool HasRecoveredCopy = false, HasSource = false;
      for (const auto &I : Block) {
        HasSource |= I.getMetadata(
                         windows_eh_md::RegistrationBlockAttachment) != nullptr;
        const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I);
        const auto *Recover = Store ? llvm::dyn_cast<llvm::IntrinsicInst>(
                                          Store->getPointerOperand())
                                    : nullptr;
        HasRecoveredCopy |= Recover && Recover->getIntrinsicID() ==
                                           llvm::Intrinsic::localrecover;
      }
      if (HasRecoveredCopy && !HasSource &&
          &Block != &Function.getEntryBlock() &&
          llvm::isa<llvm::UncondBrInst>(Block.getTerminator()))
        Sites.emplace_back(Function.getName().str(), Index);
      ++Index;
    }
  }
  for (const auto &[Name, Index] : Sites)
    for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
      SCOPED_TRACE(Name);
      SCOPED_TRACE(Index);
      SCOPED_TRACE(Mutation);
      auto Module = llvm::CloneModule(*Parent.getParent());
      auto *Owner = Module->getFunction(Parent.getName());
      auto *Callback = Module->getFunction(Name);
      auto *Block = &*std::next(Callback->begin(), Index);
      llvm::StoreInst *Store = nullptr;
      llvm::IntrinsicInst *Recover = nullptr;
      for (auto &I : *Block)
        if (auto *Candidate = llvm::dyn_cast<llvm::StoreInst>(&I))
          if (auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(
                  Candidate->getPointerOperand());
              Call && Call->getIntrinsicID() == llvm::Intrinsic::localrecover) {
            Store = Candidate;
            Recover = Call;
            break;
          }
      ASSERT_TRUE(Store && Recover);
      llvm::IRBuilder<> B(Block->getTerminator());
      switch (Mutation) {
      case 0: {
        auto *Global = new llvm::GlobalVariable(
            *Module, Store->getValueOperand()->getType(), false,
            llvm::GlobalValue::ExternalLinkage, nullptr, "copy.escape");
        Store->setOperand(1, Global);
        break;
      }
      case 1:
        Store->setAtomic(llvm::AtomicOrdering::Monotonic);
        break;
      case 2:
        B.CreateStore(B.getInt8(1), Recover);
        break;
      case 3:
        B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
            Module.get(), llvm::Intrinsic::sideeffect));
        break;
      case 4:
        Block->getTerminator()->setSuccessor(0, Block);
        break;
      case 5:
        B.CreatePtrToInt(Recover, B.getInt32Ty());
        break;
      case 6:
        Recover->setArgOperand(1, llvm::ConstantPointerNull::get(B.getPtrTy()));
        break;
      case 7:
        Recover->setArgOperand(2, B.getInt32(INT32_MAX));
        break;
      case 8:
        llvm::BlockAddress::get(Block);
        break;
      case 9: {
        auto *Other = llvm::BasicBlock::Create(Module->getContext(),
                                               "copy.extra", Callback);
        B.SetInsertPoint(Other);
        B.CreateBr(Block);
        break;
      }
      }
      ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
      auto Error = validateCOFFRegistrationIR(*Owner, Source, Image);
      EXPECT_TRUE(bool(Error));
      if (Error)
        llvm::consumeError(std::move(Error));
    }
#endif
}
} // namespace neverd::registration_test
