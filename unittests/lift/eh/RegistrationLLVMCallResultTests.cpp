//===- RegistrationLLVMCallResultTests.cpp - PE32 private uses
//-------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MedLLVMRegistrationCallResult.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/IR/Verifier.h"

namespace {
class RegistrationLLVMCallResult : public testing::Test {
protected:
  llvm::LLVMContext Context;
  llvm::Module Module{"registration-results", Context};
  llvm::IRBuilder<> Builder{Context};
  llvm::CallInst *Call = nullptr;
  llvm::AllocaInst *Slot = nullptr;

  void SetUp() override {
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(Builder.getInt32Ty(), false),
        llvm::GlobalValue::ExternalLinkage, "probe", Module);
    Builder.SetInsertPoint(
        llvm::BasicBlock::Create(Context, "entry", Function));
    Slot = Builder.CreateAlloca(Builder.getInt32Ty());
    auto *Callee = llvm::Function::Create(
        llvm::FunctionType::get(Builder.getInt64Ty(), false),
        llvm::GlobalValue::ExternalLinkage, "callback", Module);
    Call = Builder.CreateCall(Callee);
  }

  auto plan(llvm::ArrayRef<llvm::AllocaInst *> Slots) {
    size_t Work = 0;
    return neverd::med_llvm_eh::unusedRegistrationCallResultPlan(*Call, Slots,
                                                                 Work);
  }

  void TearDown() override {
    EXPECT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  }
};

TEST_F(RegistrationLLVMCallResult, RemovesPrivateTransportsAndKeepsTheCall) {
  auto *Trunc = Builder.CreateTrunc(Call, Builder.getInt32Ty());
  auto *Store = Builder.CreateStore(Trunc, Slot);
  Builder.CreateRet(Builder.getInt32(19));
  auto Plan = plan({Slot});
  ASSERT_TRUE(Plan);
  ASSERT_EQ(Plan->size(), 2u);
  EXPECT_EQ((*Plan)[0], Store);
  EXPECT_EQ((*Plan)[1], Trunc);
  llvm::WeakTrackingVH Kept(Call);
  for (auto *Instruction : *Plan) {
    ASSERT_TRUE(Instruction->use_empty());
    Instruction->eraseFromParent();
  }
  EXPECT_NE(Kept, nullptr);
  EXPECT_TRUE(Call->use_empty());
}

TEST_F(RegistrationLLVMCallResult, RejectsObservedSlotAndDirectResult) {
  auto *Trunc = Builder.CreateTrunc(Call, Builder.getInt32Ty());
  Builder.CreateStore(Trunc, Slot);
  auto *Return =
      Builder.CreateRet(Builder.CreateLoad(Builder.getInt32Ty(), Slot));
  EXPECT_FALSE(plan({Slot}));
  Return->setOperand(0, Trunc);
  EXPECT_FALSE(plan({Slot}));
}

TEST_F(RegistrationLLVMCallResult, RejectsSourceMemoryAndVolatileStores) {
  auto *Trunc = Builder.CreateTrunc(Call, Builder.getInt32Ty());
  auto *Store = Builder.CreateStore(Trunc, Slot);
  Builder.CreateRet(Builder.getInt32(19));
  EXPECT_FALSE(plan({}));
  Store->setVolatile(true);
  EXPECT_FALSE(plan({Slot}));
  Store->setVolatile(false);
  Store->setAtomic(llvm::AtomicOrdering::Monotonic);
  EXPECT_FALSE(plan({Slot}));
}

TEST_F(RegistrationLLVMCallResult, RejectsEscapedSlotsAndSourceMetadata) {
  auto *Trunc = Builder.CreateTrunc(Call, Builder.getInt32Ty());
  auto *Store = Builder.CreateStore(Trunc, Slot);
  auto *Observer = llvm::Function::Create(
      llvm::FunctionType::get(Builder.getVoidTy(), {Builder.getPtrTy()}, false),
      llvm::GlobalValue::ExternalLinkage, "observe", Module);
  auto *Escape = Builder.CreateCall(Observer, {Slot});
  Builder.CreateRet(Builder.getInt32(19));
  EXPECT_FALSE(plan({Slot}));
  Escape->eraseFromParent();
  Store->setMetadata("source-operation", llvm::MDNode::get(Context, {}));
  EXPECT_FALSE(plan({Slot}));
}

TEST_F(RegistrationLLVMCallResult, RejectsOtherUsersAndExhaustedWork) {
  auto *Trunc = Builder.CreateTrunc(Call, Builder.getInt32Ty());
  Builder.CreateStore(Builder.CreateAdd(Trunc, Builder.getInt32(1)), Slot);
  Builder.CreateRet(Builder.getInt32(19));
  EXPECT_FALSE(plan({Slot}));
  size_t Work = neverd::limits::kMaxRegistrationEHStateWork;
  EXPECT_FALSE(neverd::med_llvm_eh::unusedRegistrationCallResultPlan(
      *Call, {Slot}, Work));
}
} // namespace
