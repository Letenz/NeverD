//===- RegistrationSEHPHICopyTests.cpp - SEH copy block ownership --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/llvm/X86/MedLLVMRegistrationCopyBlocks.h"
#include "gtest/gtest.h"

#include "neverd/Common.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

using namespace neverd;

TEST(RegistrationSEHPHICopy, FilterAndFinallyInvocationsStayDistinct) {
  for (bool Finally : {false, true})
    for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
      SCOPED_TRACE(Finally);
      SCOPED_TRACE(Mutation);
      llvm::LLVMContext Context;
      llvm::Module Module("seh-copy-owner", Context);
      auto *Function = llvm::Function::Create(
          llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false),
          llvm::GlobalValue::InternalLinkage, "parent", Module);
      auto *From = llvm::BasicBlock::Create(Context, "from", Function);
      auto *Copy = llvm::BasicBlock::Create(Context, "copy", Function);
      auto *To = llvm::BasicBlock::Create(Context, "to", Function);
      llvm::IRBuilder<> Builder(From);
      Builder.CreateBr(Copy);
      Builder.SetInsertPoint(Copy);
      Builder.CreateBr(To);
      Builder.SetInsertPoint(To);
      Builder.CreateRetVoid();
      const std::pair<va_t, bool> Owner{0x1800, Finally};
      std::map<int, std::pair<va_t, bool>> Owners{{0, Owner}, {1, Owner}};
      std::map<int, llvm::BasicBlock *> Blocks{{0, From}, {1, To}};
      std::map<llvm::BasicBlock *, std::pair<int, int>> Edges{{Copy, {0, 1}}};
      if (Mutation == 1)
        Owners[1].first = 0x1900;
      if (Mutation == 2)
        Owners[1].second = !Finally;
      if (Mutation == 3)
        Owners.erase(0);
      if (Mutation == 4)
        Owners.erase(1);
      const auto Result = registrationPHICopyOwners(Owners, Blocks, Edges);
      ASSERT_EQ(bool(Result), Mutation == 0);
      if (Result) {
        ASSERT_EQ(Result->size(), 1u);
        EXPECT_EQ(Result->at(Copy), Owner);
      }
    }
}
