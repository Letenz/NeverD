#include "../lib/backend/c/LLVMC/LLVMCVariableEdits.h"
#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/c/CSourceMap.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
using namespace neverd;
namespace {
struct Fixture {
  llvm::LLVMContext Context;
  llvm::Module Module{"variable-edits", Context};
  llvm::Function *Function;
  llvm::AllocaInst *A, *B;
  llvm::IRBuilder<> Builder{Context};
  Fixture() {
    Module.setDataLayout("e-p:64:64-i64:64-n8:16:32:64");
    Function = llvm::Function::Create(
        llvm::FunctionType::get(Builder.getInt64Ty(), false),
        llvm::GlobalValue::ExternalLinkage, "lifetimes", Module);
    rewrite_source::setOriginalVA(*Function, 0x1000);
    Builder.SetInsertPoint(
        llvm::BasicBlock::Create(Context, "entry", Function));
    A = Builder.CreateAlloca(Builder.getInt64Ty(), nullptr, "a");
    B = Builder.CreateAlloca(Builder.getInt64Ty(), nullptr, "b");
  }
  void observe(llvm::Value *V) {
    auto Sink = Module.getOrInsertFunction(
        "observe", llvm::FunctionType::get(Builder.getVoidTy(),
                                           {Builder.getInt64Ty()}, false));
    Builder.CreateCall(Sink, {V});
  }
  void independent() {
    Builder.CreateStore(Builder.getInt64(7), A);
    observe(Builder.CreateLoad(Builder.getInt64Ty(), A));
    Builder.CreateStore(Builder.getInt64(9), A);
    Builder.CreateRet(Builder.CreateLoad(Builder.getInt64Ty(), A));
  }
};
TEST(LLVMVariableEdits, SplitsPrivateStorageAndProducesVerifiedSource) {
  Fixture F;
  F.independent();
  CSourceMap Map;
  CEmitterOptions Opts;
  Opts.SourceMap = &Map;
  Opts.VariableSnapshot = "baseline";
  std::vector<SourceVariableEdit> Edits{
      {"split", "instruction:0", "", "baseline", "split-profile"}};
  Opts.VariableEdits = &Edits;
  std::string Before;
  llvm::raw_string_ostream BeforeOS(Before);
  F.Module.print(BeforeOS, nullptr);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(
      LLVMCEmitter().emit(F.Module, OS, Opts, nullptr, nullptr, F.Function));
  ASSERT_EQ(Map.VariableEditResults.size(), 1u);
  ASSERT_TRUE(Map.VariableEditResults[0].Applied)
      << Map.VariableEditResults[0].Refusal;
  EXPECT_EQ(Map.VariableEditResults[0].Outputs.size(), 2u);
  EXPECT_EQ(Map.VariableSnapshot, "split-profile");
  EXPECT_NE(Source.find("local_part"), std::string::npos) << Source;
  std::string After;
  llvm::raw_string_ostream AfterOS(After);
  F.Module.print(AfterOS, nullptr);
  EXPECT_EQ(Before, After);
}
TEST(LLVMVariableEdits, MapsCompleteScalarStorageCopies) {
  Fixture F;
  F.Builder.CreateStore(F.Builder.getInt64(7), F.A);
  F.Builder.CreateStore(F.Builder.CreateLoad(F.Builder.getInt64Ty(), F.A), F.B);
  F.Builder.CreateRet(F.Builder.CreateLoad(F.Builder.getInt64Ty(), F.B));
  llvmc::captureSourceValueIds(*F.Function);
  auto R = llvmc::editVariableStorage(
      *F.Function, {"map", "instruction:0", "instruction:1"}, 0);
  ASSERT_TRUE(R.Applied) << R.Refusal;
  EXPECT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
}
TEST(LLVMVariableEdits, RefusesEscapedPartialAndSSASplitsWithoutMutation) {
  Fixture F;
  F.independent();
  llvmc::captureSourceValueIds(*F.Function);
  F.Builder.SetInsertPoint(F.Function->getEntryBlock().getTerminator());
  auto Sink = F.Module.getOrInsertFunction(
      "escape", llvm::FunctionType::get(F.Builder.getVoidTy(),
                                        {F.Builder.getPtrTy()}, false));
  F.Builder.CreateCall(Sink, {F.A});
  auto R =
      llvmc::editVariableStorage(*F.Function, {"split", "instruction:0"}, 0);
  EXPECT_FALSE(R.Applied);
  EXPECT_NE(R.Refusal.find("escapes"), std::string::npos);
  R = llvmc::editVariableStorage(*F.Function, {"split", "instruction:3"}, 0);
  EXPECT_FALSE(R.Applied);
  EXPECT_NE(R.Refusal.find("SSA"), std::string::npos);
  F.Function->getEntryBlock().getTerminator()->getPrevNode()->eraseFromParent();
  F.Builder.CreateStore(F.Builder.getInt32(1), F.A);
  R = llvmc::editVariableStorage(*F.Function, {"split", "instruction:0"}, 0);
  EXPECT_FALSE(R.Applied);
  EXPECT_NE(R.Refusal.find("partial"), std::string::npos);
}
} // namespace
