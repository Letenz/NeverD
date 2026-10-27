//===- RegistrationSEHParentExitIRMutations.cpp - Parent exit edits ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Rejects verifier-clean changes to the native parent context contract.

#include "RegistrationSEHParentExitIRMutations.h"

#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"

namespace neverd::registration_test {

llvm::Instruction *sourceSetupMutationPoint(llvm::Function &Function) {
  for (auto &I : Function.getEntryBlock())
    if (auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&I);
        Call && Call->getIntrinsicID() == llvm::Intrinsic::localescape)
      return Call;
  return Function.getEntryBlock().getTerminator();
}

void checkSEHParentExitEdits(const llvm::Function &Parent,
                             const ExceptionFunction &Source,
                             const BinaryImage &Image) {
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
  if (llvm::none_of(Parent.getEntryBlock(), [](const auto &I) {
        return I.getMetadata(windows_eh_md::RegistrationParentExitAttachment);
      }))
    return;
  for (unsigned Mutation = 0; Mutation != 36; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Module = llvm::CloneModule(*Parent.getParent());
    auto *Function = Module->getFunction(Parent.getName());
    llvm::AllocaInst *Slots[3]{};
    llvm::CallInst *Capture = nullptr, *Transfer = nullptr;
    for (auto &Owner : *Module)
      for (auto &Block : Owner)
        for (auto &I : Block)
          if (const auto *MD = I.getMetadata(
                  windows_eh_md::RegistrationParentExitAttachment)) {
            const auto Role =
                llvm::mdconst::extract<llvm::ConstantInt>(MD->getOperand(2))
                    ->getZExtValue();
            if (Role < 3)
              Slots[Role] = llvm::cast<llvm::AllocaInst>(&I);
            else if (Role == 4)
              Capture = llvm::cast<llvm::CallInst>(&I);
            else
              Transfer = llvm::cast<llvm::CallInst>(&I);
          }
    ASSERT_TRUE(Slots[0] && Slots[1] && Slots[2] && Capture && Transfer);
    auto *Compare = llvm::cast<llvm::ICmpInst>(Capture->getNextNode());
    auto *EntryBranch = llvm::cast<llvm::CondBrInst>(Compare->getNextNode());
    auto *Dispatch = llvm::cast<llvm::SwitchInst>(
        EntryBranch->getSuccessor(1)->getTerminator());
    auto *Selected = llvm::cast<llvm::LoadInst>(Dispatch->getCondition());
    auto *SaveStack = llvm::cast<llvm::StoreInst>(Capture->getPrevNode());
    auto *StackCell =
        llvm::cast<llvm::GetElementPtrInst>(SaveStack->getPointerOperand());
    auto *Stack = llvm::cast<llvm::CallInst>(StackCell->getPrevNode());
    auto *SaveFrame = llvm::cast<llvm::StoreInst>(Stack->getPrevNode());
    auto *Frame = llvm::cast<llvm::CallInst>(SaveFrame->getValueOperand());
    auto *SaveHead = llvm::cast<llvm::StoreInst>(Frame->getPrevNode());
    auto *Head = llvm::cast<llvm::LoadInst>(SaveHead->getValueOperand());
    auto *Restore = llvm::cast<llvm::StoreInst>(Transfer->getPrevNode());
    auto *Chain = llvm::cast<llvm::LoadInst>(Restore->getValueOperand());
    auto *Select = llvm::cast<llvm::StoreInst>(Chain->getPrevNode());
    llvm::IRBuilder<> B(Transfer);
    auto EditMarker = [&](llvm::Instruction *I, unsigned Field) {
      const auto *MD =
          I->getMetadata(windows_eh_md::RegistrationParentExitAttachment);
      llvm::SmallVector<llvm::Metadata *, 3> Values;
      for (const auto &Value : MD->operands())
        Values.push_back(Value.get());
      const auto *Old =
          llvm::mdconst::extract<llvm::ConstantInt>(Values[Field]);
      Values[Field] =
          llvm::ConstantAsMetadata::get(B.getInt64(Old->getZExtValue() + 1));
      I->setMetadata(windows_eh_md::RegistrationParentExitAttachment,
                     llvm::MDNode::get(Module->getContext(), Values));
    };
    switch (Mutation) {
    case 0:
      Capture->setMetadata(windows_eh_md::RegistrationParentExitAttachment,
                           nullptr);
      break;
    case 1:
      EditMarker(Slots[0], 0);
      break;
    case 2:
      EditMarker(Slots[0], 2);
      break;
    case 3:
      EditMarker(Transfer, 1);
      break;
    case 4:
      EditMarker(Capture, 1);
      break;
    case 5:
      Slots[0]->setAllocatedType(llvm::ArrayType::get(B.getPtrTy(), 6));
      break;
    case 6:
      Slots[1]->setAlignment(llvm::Align(8));
      break;
    case 7:
      StackCell->setOperand(1, B.getInt32(12));
      break;
    case 8:
      Frame->setArgOperand(0, B.getInt32(1));
      break;
    case 9:
      Head->setOperand(0, llvm::ConstantPointerNull::get(B.getPtrTy()));
      break;
    case 10:
      Capture->setArgOperand(0, Slots[1]);
      break;
    case 11:
      Compare->setPredicate(llvm::CmpInst::ICMP_NE);
      break;
    case 12:
      EntryBranch->swapSuccessors();
      break;
    case 13:
      Dispatch->setDefaultDest(EntryBranch->getSuccessor(0));
      break;
    case 14:
      Selected->setOperand(0, Slots[1]);
      break;
    case 15:
      Capture->clone()->insertBefore(Capture->getIterator());
      break;
    case 16:
      Capture->moveBefore(Head->getIterator());
      break;
    case 17:
      SaveHead->setOperand(0, B.getInt32(0));
      break;
    case 18:
      Select->setOperand(0, B.getInt32(INT_MAX));
      break;
    case 19:
      Chain->setOperand(0, Select->getPointerOperand());
      break;
    case 20:
      Restore->setOperand(0, B.getInt32(0));
      break;
    case 21:
      Transfer->setArgOperand(0, Chain->getPointerOperand());
      break;
    case 22:
      Transfer->removeFnAttr(llvm::Attribute::NoReturn);
      break;
    case 23:
      Transfer->setTailCall(true);
      break;
    case 24:
      Transfer->setMetadata(windows_eh_md::RegistrationParentExitAttachment,
                            nullptr);
      break;
    case 25:
      Transfer->clone()->insertBefore(Transfer->getIterator());
      break;
    case 26:
      EditMarker(Transfer, 0);
      break;
    case 27:
      Dispatch->case_begin()->setSuccessor(EntryBranch->getSuccessor(0));
      break;
    case 28:
      B.SetInsertPoint(sourceSetupMutationPoint(*Function));
      B.CreateLoad(B.getInt32Ty(), Slots[0])->setVolatile(true);
      break;
    case 29:
      B.CreateStore(B.getInt32(0), Transfer->getArgOperand(0));
      break;
    case 30: {
      auto *Foreign = llvm::Function::Create(
          llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy()}, false),
          llvm::GlobalValue::InternalLinkage, "foreign.recovery", *Module);
      B.SetInsertPoint(
          llvm::BasicBlock::Create(B.getContext(), "entry", Foreign));
      auto *Recover =
          llvm::cast<llvm::IntrinsicInst>(Transfer->getArgOperand(0));
      B.CreateCall(Recover->getCalledFunction(),
                   {Function, Foreign->getArg(0), Recover->getArgOperand(2)});
      B.CreateRetVoid();
      break;
    }
    case 31:
      B.CreateLoad(B.getInt32Ty(), Restore->getPointerOperand())
          ->setVolatile(true);
      break;
    case 32:
      Restore->eraseFromParent();
      break;
    case 33: {
      auto *Previous = Transfer->getParent()->getSinglePredecessor();
      ASSERT_TRUE(Previous);
      B.SetInsertPoint(Previous->getTerminator());
      B.CreateRetVoid();
      Previous->getTerminator()->eraseFromParent();
      break;
    }
    case 34:
      B.SetInsertPoint(sourceSetupMutationPoint(*Function));
      B.CreateStore(B.getInt32(0), Slots[2]);
      break;
    case 35:
      Module->getNamedGlobal("llvm.compiler.used")->eraseFromParent();
      break;
    }
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    auto Error = validateCOFFRegistrationIR(*Function, Source, Image);
    ASSERT_TRUE(bool(Error));
    const auto Message = llvm::toString(std::move(Error));
    EXPECT_TRUE(Message.find("parent") != std::string::npos ||
                Message.find("callback") != std::string::npos)
        << Message;
  }
#endif
}

} // namespace neverd::registration_test
