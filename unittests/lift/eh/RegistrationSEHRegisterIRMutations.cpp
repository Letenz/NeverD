//===- RegistrationSEHRegisterIRMutations.cpp - Register exchange edits --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Rejects verifier-clean changes to finally register storage and execution.

#include "RegistrationSEHRegisterIRMutations.h"

#include "RegistrationSEHParentExitIRMutations.h"
#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"

namespace neverd::registration_test {

void checkSEHRegisterEdits(const llvm::Function &Parent,
                           const ExceptionFunction &Source,
                           const BinaryImage &Image) {
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
  if (llvm::none_of(Parent.getEntryBlock(), [](const auto &I) {
        return I.getMetadata(windows_eh_md::RegistrationRegisterAttachment);
      }))
    return;
  for (unsigned Mutation = 0; Mutation != 30; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Module = llvm::CloneModule(*Parent.getParent());
    auto *Function = Module->getFunction(Parent.getName());
    llvm::AllocaInst *Slot = nullptr;
    llvm::LoadInst *Input = nullptr, *Output = nullptr;
    llvm::StoreInst *Save = nullptr, *Publish = nullptr;
    const auto Marker = windows_eh_md::RegistrationRegisterAttachment;
    for (auto &Owner : *Module)
      for (auto &Block : Owner)
        for (auto &I : Block)
          if (const auto *MD = I.getMetadata(Marker)) {
            const auto Role =
                llvm::mdconst::extract<llvm::ConstantInt>(MD->getOperand(1))
                    ->getZExtValue();
            if (Role == 4)
              Slot = llvm::cast<llvm::AllocaInst>(&I);
            else if (Role == 0)
              Save = llvm::cast<llvm::StoreInst>(&I);
            else if (Role == 1)
              Input = llvm::cast<llvm::LoadInst>(&I);
            else if (Role == 3)
              Output = llvm::cast<llvm::LoadInst>(&I);
            else if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I);
                     Store &&
                     llvm::isa<llvm::ConstantInt>(Store->getValueOperand()))
              Publish = Store;
          }
    ASSERT_TRUE(Slot && Input && Save);
    auto *Guard = llvm::cast<llvm::SelectInst>(*Input->user_begin());
    auto *Compare = llvm::cast<llvm::ICmpInst>(Guard->getCondition());
    llvm::IRBuilder<> B(Input);
    const auto Edit = [&](llvm::Instruction *I, unsigned Field) {
      llvm::SmallVector<llvm::Metadata *, 5> Values;
      for (const auto &Value : I->getMetadata(Marker)->operands())
        Values.push_back(Value.get());
      const auto Old = llvm::mdconst::extract<llvm::ConstantInt>(Values[Field])
                           ->getZExtValue();
      Values[Field] = llvm::ConstantAsMetadata::get(B.getInt64(Old + 1));
      I->setMetadata(Marker, llvm::MDNode::get(B.getContext(), Values));
    };
    if (Mutation >= 1 && Mutation <= 5)
      Edit(Slot, Mutation - 1);
    else if (Mutation >= 12 && Mutation <= 16)
      Edit(Input, Mutation - 12);
    else
      switch (Mutation) {
      case 0:
        Slot->setMetadata(Marker, nullptr);
        break;
      case 6:
        Slot->setAllocatedType(B.getInt64Ty());
        break;
      case 7:
        Slot->setAlignment(llvm::Align(8));
        break;
      case 8:
        B.SetInsertPoint(sourceSetupMutationPoint(*Function));
        B.CreateLoad(B.getInt32Ty(), Slot)->setVolatile(true);
        break;
      case 9:
        B.SetInsertPoint(sourceSetupMutationPoint(*Function));
        B.CreatePtrToInt(Slot, B.getInt32Ty());
        break;
      case 10:
        Input->setOperand(0, B.CreateAlloca(B.getInt32Ty()));
        break;
      case 11:
        B.SetInsertPoint(Save);
        Save->setOperand(1, B.CreateAlloca(B.getInt32Ty()));
        break;
      case 17:
        Input->setMetadata(Marker, nullptr);
        break;
      case 18:
        Input->setVolatile(false);
        break;
      case 19:
        Save->setVolatile(false);
        break;
      case 20:
        Input->setAlignment(llvm::Align(8));
        break;
      case 21:
        Guard->setFalseValue(Guard->getTrueValue());
        break;
      case 22:
        Compare->setPredicate(llvm::CmpInst::ICMP_EQ);
        break;
      case 23:
        Compare->setOperand(1, B.getInt8(1));
        break;
      case 24:
        Guard->setTrueValue(B.getInt32(0));
        break;
      case 25:
        B.Insert(Input->clone());
        break;
      case 26:
        Save->eraseFromParent();
        break;
      case 27:
      case 28: {
        auto *Access =
            Mutation == 27 ? static_cast<llvm::Instruction *>(Save) : Output;
        if (!Access)
          continue;
        const auto Site = llvm::mdconst::extract<llvm::ConstantInt>(
                              Access->getMetadata(Marker)->getOperand(3))
                              ->getZExtValue();
        llvm::CallBase *Call = nullptr;
        for (auto &Block : *Access->getFunction())
          for (auto &I : Block)
            if (const auto *MD = I.getMetadata(
                    windows_eh_md::RegistrationFinallyCallAttachment);
                MD &&
                llvm::mdconst::extract<llvm::ConstantInt>(MD->getOperand(1))
                        ->getZExtValue() == Site)
              Call = llvm::cast<llvm::CallBase>(&I);
        ASSERT_TRUE(Call);
        if (Mutation == 28)
          Access->moveBefore(Call->getIterator());
        else if (auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(Call))
          Access->moveBefore(Invoke->getNormalDest()->getFirstInsertionPt());
        else
          Access->moveAfter(Call->getIterator());
        break;
      }
      case 29:
        if (!Publish)
          continue;
        Publish->setOperand(0, B.getInt32(llvm::cast<llvm::ConstantInt>(
                                              Publish->getValueOperand())
                                              ->getZExtValue() +
                                          1));
        break;
      }
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    auto Error = validateCOFFRegistrationIR(*Function, Source, Image);
    ASSERT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
#endif
}

} // namespace neverd::registration_test
