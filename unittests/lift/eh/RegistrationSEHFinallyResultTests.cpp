//===- RegistrationSEHFinallyResultTests.cpp - Unobserved call values ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/llvm/X86/MedLLVMRegistrationSEHProof.h"
#include "gtest/gtest.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"

using namespace neverd;

TEST(RegistrationSEHFinallyResult, FollowsOnlyUnobservablePrivateSpills) {
  for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
    SCOPED_TRACE(Mutation);
    llvm::LLVMContext Context;
    llvm::Module Module("finally-result", Context);
    llvm::IRBuilder<> B(Context);
    auto *Type = llvm::FunctionType::get(B.getInt32Ty(), false);
    auto *Function = llvm::Function::Create(
        Type, llvm::GlobalValue::ExternalLinkage, "parent", Module);
    auto *Callee = llvm::Function::Create(
        Type, llvm::GlobalValue::ExternalLinkage, "finally", Module);
    B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Function));
    auto *First = B.CreateAlloca(B.getInt32Ty());
    auto *Last = B.CreateAlloca(B.getInt32Ty());
    auto *Call = B.CreateCall(Callee);
    auto *Store = B.CreateStore(Call, First);
    Store->setVolatile(Mutation == 5);
    auto *Read =
        B.CreateLoad(Mutation == 6 ? B.getInt64Ty() : B.getInt32Ty(), First);
    Read->setVolatile(Mutation == 1);
    if (Mutation == 2)
      Read->setAtomic(llvm::AtomicOrdering::Unordered);
    if (Mutation == 3)
      B.CreateStore(B.getInt32(1),
                    B.CreateGEP(B.getInt8Ty(), First, B.getInt32(0)));
    B.CreateStore(Read, Last);
    llvm::Value *Result =
        Mutation == 4 ? static_cast<llvm::Value *>(Read) : B.getInt32(0);
    B.CreateRet(Result);
    ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    std::vector<llvm::Instruction *> Dead;
    EXPECT_EQ(x86_registration::collectDeadFinallyResult(*Call, Dead),
              Mutation == 0);
    if (!Mutation)
      EXPECT_EQ(Dead.size(), 3u);
  }
}
