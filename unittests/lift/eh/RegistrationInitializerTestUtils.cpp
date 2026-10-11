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

namespace neverd::registration_test {
void checkInitializerEdits(const MedFunc &Med, const llvm::Function &Parent,
                           const BinaryImage &Image) {
  for (const auto &Contract : Med.RegistrationStates->CalleeContracts) {
    if (!Contract.isInitializer())
      continue;
    for (bool MissingArgument : {false, true}) {
      auto Changed = Med;
      auto Call = llvm::find_if(Changed.CallInfos, [&](const auto &Candidate) {
        return Candidate.TargetAddr == Contract.Target;
      });
      ASSERT_NE(Call, Changed.CallInfos.end());
      ASSERT_GT(Call->Args.size(), 1u);
      if (MissingArgument)
        Call->Args.pop_back();
      else
        Call->Args[0] = MedVar::makeConst(1, 4);
      llvm::LLVMContext Context;
      auto Module =
          MedLLVMEmitter().emit({Changed}, Context, "edited-initializer",
                                Arch::X86, {}, &Image, BinaryFormat::COFF);
      ASSERT_TRUE(Module);
      const auto *Function = Module->getFunction(Changed.Name);
      ASSERT_TRUE(Function);
      EXPECT_FALSE(Function->getMetadata(windows_eh_md::NativeAttachment));
    }
    for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
      SCOPED_TRACE(Contract.Target);
      SCOPED_TRACE(Mutation);
      auto Module = llvm::CloneModule(*Parent.getParent());
      auto *Function = Module->getFunction(Parent.getName());
      llvm::CallBase *Call = nullptr;
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
          if (*Target == Contract.Target) {
            ASSERT_EQ(Call, nullptr);
            Call = Candidate;
          }
        }
      ASSERT_NE(Call, nullptr);
      ASSERT_GT(Call->arg_size(), 1u);
      llvm::IRBuilder<> B(Call);
      if (Mutation == 1)
        Call->setArgOperand(0, llvm::ConstantPointerNull::get(B.getPtrTy()));
      if (Mutation == 2)
        Call->setArgOperand(
            0,
            B.CreateGEP(B.getInt8Ty(), Call->getArgOperand(0), B.getInt32(1)));
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
      auto Error = validateCOFFRegistrationCxxIR(*Function,
                                                 *Med.ExceptionMetadata, Image);
      const bool Failed = bool(Error);
      EXPECT_EQ(Failed, Mutation != 0) << llvm::toString(std::move(Error));
      llvm::consumeError(std::move(Error));
    }
  }
}
} // namespace neverd::registration_test
