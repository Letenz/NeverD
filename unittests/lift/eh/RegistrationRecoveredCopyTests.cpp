//===- RegistrationRecoveredCopyTests.cpp - Outlined copy storage --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/codegen/COFF/COFFRegistrationCopyEdges.h"
#include "../../../lib/backend/codegen/COFF/COFFRegistrationCopySlots.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"

using namespace neverd;

TEST(RegistrationRecoveredCopy, RequiresExactRecoveryAndPrivateScalarUses) {
  for (bool Filter : {false, true})
    for (unsigned Mutation = 0; Mutation != 15; ++Mutation) {
      SCOPED_TRACE(Filter);
      SCOPED_TRACE(Mutation);
      llvm::LLVMContext Context;
      llvm::Module Module("recovered-copy", Context);
      llvm::IRBuilder<> B(Context);
      auto *Parent = llvm::Function::Create(
          llvm::FunctionType::get(B.getInt32Ty(), false),
          llvm::GlobalValue::ExternalLinkage, "parent", Module);
      auto *Setup = llvm::BasicBlock::Create(Context, "setup", Parent);
      B.SetInsertPoint(Setup);
      auto *Slot =
          B.CreateAlloca(Mutation == 4 ? B.getInt64Ty() : B.getInt32Ty());
      auto *Escape = B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                                      &Module, llvm::Intrinsic::localescape),
                                  {Slot});
      B.CreateRet(B.getInt32(0));
      auto *CallbackType =
          Filter ? llvm::FunctionType::get(B.getInt32Ty(), false)
                 : llvm::FunctionType::get(
                       B.getVoidTy(), {B.getInt8Ty(), B.getPtrTy()}, false);
      auto *Callback = llvm::Function::Create(
          CallbackType, llvm::GlobalValue::InternalLinkage, "callback", Module);
      auto *Entry = llvm::BasicBlock::Create(Context, "entry", Callback);
      auto *Copy = llvm::BasicBlock::Create(Context, "copy", Callback);
      auto *Target = llvm::BasicBlock::Create(Context, "target", Callback);
      B.SetInsertPoint(Entry);
      llvm::Value *Frame = nullptr;
      if (Filter) {
        auto *Runtime = B.CreateCall(
            llvm::Intrinsic::getOrInsertDeclaration(
                &Module, llvm::Intrinsic::frameaddress, {B.getPtrTy()}),
            {B.getInt32(1)});
        Frame = B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                                 &Module, llvm::Intrinsic::eh_recoverfp),
                             {Parent, Runtime});
      } else
        Frame = Callback->getArg(1);
      auto *RecoverDecl = llvm::Intrinsic::getOrInsertDeclaration(
          &Module, llvm::Intrinsic::localrecover);
      auto *Recover = B.CreateCall(RecoverDecl, {Parent, Frame, B.getInt32(0)});
      auto *Branch = B.CreateBr(Copy);
      B.SetInsertPoint(Copy);
      auto *Read = B.CreateLoad(B.getInt32Ty(), Recover);
      auto *Store = B.CreateStore(Read, Recover);
      B.CreateBr(Target);
      B.SetInsertPoint(Target);
      if (Filter)
        B.CreateRet(B.getInt32(-1));
      else
        B.CreateRetVoid();
      if (Mutation == 1) {
        auto *Other = llvm::Function::Create(Parent->getFunctionType(),
                                             llvm::GlobalValue::ExternalLinkage,
                                             "other_parent", Module);
        Recover->setArgOperand(0, Other);
      }
      if (Mutation == 2)
        Recover->setArgOperand(1, llvm::ConstantPointerNull::get(B.getPtrTy()));
      if (Mutation == 3)
        Recover->setArgOperand(2, B.getInt32(1));
      if (Mutation == 5 || Mutation == 6) {
        B.SetInsertPoint(Branch);
        if (Mutation == 5) {
          auto *Global = new llvm::GlobalVariable(
              Module, B.getPtrTy(), false, llvm::GlobalValue::ExternalLinkage,
              nullptr, "escaped");
          B.CreateStore(Recover, Global);
        } else
          B.CreatePtrToInt(Recover, B.getInt32Ty());
      }
      if (Mutation == 7) {
        auto *Other = llvm::Function::Create(CallbackType,
                                             llvm::GlobalValue::InternalLinkage,
                                             "unowned", Module);
        auto *Block = llvm::BasicBlock::Create(Context, "entry", Other);
        B.SetInsertPoint(Block);
        B.CreateCall(RecoverDecl,
                     {Parent, llvm::ConstantPointerNull::get(B.getPtrTy()),
                      B.getInt32(0)});
        if (Filter)
          B.CreateRet(B.getInt32(0));
        else
          B.CreateRetVoid();
      }
      if (Mutation == 8) {
        B.SetInsertPoint(Setup->getTerminator());
        B.CreateLoad(B.getInt64Ty(), Slot);
      }
      if (Mutation == 9)
        Store->setAtomic(llvm::AtomicOrdering::Monotonic);
      if (Mutation == 10)
        Read->setVolatile(true);
      if (Mutation == 11) {
        B.SetInsertPoint(Setup->getTerminator());
        B.Insert(Escape->clone());
      }
      if (Mutation == 12)
        Recover->moveBefore(Read->getIterator());
      if (Mutation == 14) {
        auto *Global = new llvm::GlobalVariable(
            Module, B.getInt32Ty(), false, llvm::GlobalValue::ExternalLinkage,
            nullptr, "source_memory");
        Store->setOperand(1, Global);
      }
      if (Mutation == 0)
        ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
      size_t Work = Mutation == 13 ? limits::kMaxRegistrationEHStateWork : 0;
      coff_registration::RegistrationCopySlots Slots;
      const bool Built = Slots.build(*Parent, {Callback}, Work);
      const auto *Destination =
          Built ? coff_registration::registrationCopyDestination(
                      *Entry, *Copy, {Target}, Work, false, &Slots)
                : nullptr;
      EXPECT_EQ(Destination, Mutation == 0 ? Target : nullptr);
      if (Mutation == 10) {
        ASSERT_TRUE(Built);
        EXPECT_EQ(coff_registration::registrationCopyDestination(
                      *Entry, *Copy, {Target}, Work, true, &Slots),
                  Target);
      }
    }
}

TEST(RegistrationRecoveredCopy, FrozenPoisonIncludesParentObservers) {
  for (bool Observed : {false, true}) {
    llvm::LLVMContext Context;
    llvm::Module Module("recovered-poison", Context);
    llvm::IRBuilder<> B(Context);
    auto *Parent = llvm::Function::Create(
        llvm::FunctionType::get(B.getInt32Ty(), false),
        llvm::GlobalValue::ExternalLinkage, "parent", Module);
    auto *Setup = llvm::BasicBlock::Create(Context, "setup", Parent);
    B.SetInsertPoint(Setup);
    auto *Slot = B.CreateAlloca(B.getInt32Ty());
    B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                     &Module, llvm::Intrinsic::localescape),
                 {Slot});
    auto *Read = B.CreateLoad(B.getInt32Ty(), Slot);
    B.CreateRet(Observed ? static_cast<llvm::Value *>(Read) : B.getInt32(0));
    auto *Callback = llvm::Function::Create(
        llvm::FunctionType::get(B.getVoidTy(), {B.getInt8Ty(), B.getPtrTy()},
                                false),
        llvm::GlobalValue::InternalLinkage, "callback", Module);
    auto *Entry = llvm::BasicBlock::Create(Context, "entry", Callback);
    auto *Copy = llvm::BasicBlock::Create(Context, "copy", Callback);
    auto *Target = llvm::BasicBlock::Create(Context, "target", Callback);
    B.SetInsertPoint(Entry);
    auto *Recover = B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                                     &Module, llvm::Intrinsic::localrecover),
                                 {Parent, Callback->getArg(1), B.getInt32(0)});
    B.CreateBr(Copy);
    B.SetInsertPoint(Copy);
    auto *Frozen = new llvm::FreezeInst(llvm::PoisonValue::get(B.getInt32Ty()));
    B.Insert(Frozen);
    B.CreateStore(Frozen, Recover);
    B.CreateBr(Target);
    B.SetInsertPoint(Target);
    B.CreateRetVoid();
    ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    size_t Work = 0;
    coff_registration::RegistrationCopySlots Slots;
    ASSERT_TRUE(Slots.build(*Parent, {Callback}, Work));
    EXPECT_EQ(coff_registration::registrationCopyDestination(
                  *Entry, *Copy, {Target}, Work, false, &Slots),
              Observed ? nullptr : Target);
  }
}
