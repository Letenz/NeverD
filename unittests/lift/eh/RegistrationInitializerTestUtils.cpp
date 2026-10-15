//===- RegistrationInitializerTestUtils.cpp - Constructor edits -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reject changed constructor object borrows, argument domains and ABI claims.
//===----------------------------------------------------------------------===//
#include "RegistrationInitializerTestUtils.h"

#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <vector>

namespace neverd::registration_test {
void checkInitializerEdits(const MedFunc &Med, const llvm::Function &Parent,
                           const BinaryImage &Image) {
  for (const auto &Contract : Med.RegistrationStates->CalleeContracts) {
    if (!Contract.isInitializer())
      continue;
    std::vector<size_t> CallIndices;
    for (size_t I = 0; I < Med.CallInfos.size(); ++I)
      if (Med.CallInfos[I].TargetAddr == Contract.Target)
        CallIndices.push_back(I);
    ASSERT_FALSE(CallIndices.empty());
    for (size_t Index : CallIndices)
      for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
        SCOPED_TRACE(Index);
        SCOPED_TRACE(Mutation);
        auto Changed = Med;
        auto &Call = Changed.CallInfos[Index];
        ASSERT_GT(Call.Args.size(), 1u);
        if (Mutation == 1)
          Call.Args.pop_back();
        else if (Mutation == 2)
          Call.Args.push_back(MedVar::makeConst(1, 4));
        else
          Call.Args[0] = MedVar::makeConst(1, 4);
        llvm::LLVMContext Context;
        auto Module =
            MedLLVMEmitter().emit({Changed}, Context, "edited-initializer",
                                  Arch::X86, {}, &Image, BinaryFormat::COFF);
        ASSERT_TRUE(Module);
        const auto *Function = Module->getFunction(Changed.Name);
        ASSERT_TRUE(Function);
        EXPECT_FALSE(Function->getMetadata(windows_eh_md::NativeAttachment));
      }
    // A constructor can be called both from the parent and a catch callback.
    // Mutate every occurrence so the callback call cannot borrow the parent's
    // successful object/argument proof merely by sharing its target address.
    for (size_t Index = 0; Index < CallIndices.size(); ++Index)
      for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
        SCOPED_TRACE(Contract.Target);
        SCOPED_TRACE(Index);
        SCOPED_TRACE(Mutation);
        auto Module = llvm::CloneModule(*Parent.getParent());
        auto *Function = Module->getFunction(Parent.getName());
        std::vector<llvm::CallBase *> Calls;
        for (auto &Block : *Function)
          for (auto &I : Block) {
            auto *Candidate = llvm::dyn_cast<llvm::CallBase>(&I);
            auto *Callee = Candidate ? Candidate->getCalledFunction() : nullptr;
            if (!Callee || Callee->isIntrinsic())
              continue;
            auto Target = rewrite_source::getOriginalVA(*Callee);
            if (!Target) {
              llvm::consumeError(Target.takeError());
              continue;
            }
            if (*Target == Contract.Target)
              Calls.push_back(Candidate);
          }
        ASSERT_EQ(Calls.size(), CallIndices.size());
        auto *Call = Calls[Index];
        ASSERT_GT(Call->arg_size(), 1u);
        llvm::IRBuilder<> B(Call);
        if (Mutation == 1)
          Call->setArgOperand(0, llvm::ConstantPointerNull::get(B.getPtrTy()));
        if (Mutation == 2)
          Call->setArgOperand(0,
                              B.CreateGEP(B.getInt8Ty(), Call->getArgOperand(0),
                                          B.getInt32(1)));
        if (Mutation == 3)
          Call->setArgOperand(
              1, B.CreatePtrToInt(Call->getArgOperand(0), B.getInt32Ty()));
        if (Mutation == 4)
          Call->setArgOperand(1, llvm::PoisonValue::get(B.getInt32Ty()));
        if (Mutation == 5)
          Call->addParamAttr(1, llvm::Attribute::InReg);
        if (Mutation == 6)
          Call->getCalledFunction()->setMemoryEffects(
              llvm::MemoryEffects::none());
        if (Mutation == 7)
          Call->setDoesNotThrow();
        ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
        auto Error = validateCOFFRegistrationCxxIR(
            *Function, *Med.ExceptionMetadata, Image);
        const bool Failed = bool(Error);
        EXPECT_EQ(Failed, Mutation != 0) << llvm::toString(std::move(Error));
        llvm::consumeError(std::move(Error));
      }
  }
}
} // namespace neverd::registration_test
