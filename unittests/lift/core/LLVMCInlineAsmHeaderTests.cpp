//===- LLVMCInlineAsmHeaderTests.cpp - Intrinsic declarations -------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"

using namespace neverd;

namespace {

std::string emitSystemAsm(bool Read, Arch Architecture = Arch::X64,
                          bool EmitIncludes = true) {
  llvm::LLVMContext Context;
  llvm::Module Module("isolated_system_asm", Context);
  llvm::IRBuilder<> Builder(Context);
  auto *Word = Builder.getInt32Ty();
  auto *Type =
      Read ? llvm::FunctionType::get(Builder.getInt64Ty(), {Word}, false)
           : llvm::FunctionType::get(Builder.getVoidTy(), {Word, Word, Word},
                                     false);
  auto *Function =
      llvm::Function::Create(Type, llvm::GlobalValue::ExternalLinkage,
                             Read ? "read_msr" : "write_msr", Module);
  Builder.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Function));
  auto *Asm = llvm::InlineAsm::get(
      Type, Read ? "rdmsr" : "wrmsr",
      Read ? "={rax},{ecx},~{rdx}" : "{ecx},{eax},{edx}", true);
  llvm::SmallVector<llvm::Value *, 3> Args;
  for (auto &Argument : Function->args())
    Args.push_back(&Argument);
  auto *Call = Builder.CreateCall(Asm, Args);
  if (Read)
    Builder.CreateRet(Call);
  else
    Builder.CreateRetVoid();
  CEmitterOptions Options;
  Options.TheArch = Architecture;
  Options.Format = BinaryFormat::COFF;
  Options.EmitIncludes = EmitIncludes;
  std::string Source;
  llvm::raw_string_ostream Stream(Source);
  EXPECT_TRUE(LLVMCEmitter().emit(Module, Stream, Options));
  return Source;
}

TEST(LLVMCInlineAsmHeaders, IsolatedReadDeclaresItsIntrinsicBeforeUse) {
  for (auto Architecture : {Arch::X86, Arch::X64}) {
    auto Source = emitSystemAsm(true, Architecture);
    auto Header = Source.find("#include <intrin.h>");
    auto Call = Source.find("__readmsr(");
    ASSERT_NE(Call, std::string::npos) << Source;
    ASSERT_NE(Header, std::string::npos) << Source;
    EXPECT_LT(Header, Call);
    EXPECT_EQ(Source.find("#include <intrin.h>", Header + 1),
              std::string::npos);
  }
}

TEST(LLVMCInlineAsmHeaders, IsolatedWriteDeclaresItsIntrinsicBeforeUse) {
  auto Source = emitSystemAsm(false);
  auto Header = Source.find("#include <intrin.h>");
  auto Call = Source.find("__writemsr(");
  ASSERT_NE(Call, std::string::npos) << Source;
  ASSERT_NE(Header, std::string::npos) << Source;
  EXPECT_LT(Header, Call);
}

TEST(LLVMCInlineAsmHeaders, ExplicitIncludeSuppressionIsPreserved) {
  auto Source = emitSystemAsm(true, Arch::X64, false);
  EXPECT_NE(Source.find("__readmsr("), std::string::npos);
  EXPECT_EQ(Source.find("#include"), std::string::npos);
}

TEST(LLVMCInlineAsmHeaders, ExistingARMBarrierDeclarationsRemainAvailable) {
  for (auto Architecture : {Arch::ARM, Arch::AArch64}) {
    llvm::LLVMContext Context;
    llvm::Module Module("arm_barrier", Context);
    llvm::IRBuilder<> Builder(Context);
    auto *Type = llvm::FunctionType::get(Builder.getVoidTy(), false);
    auto *Function = llvm::Function::Create(
        Type, llvm::GlobalValue::ExternalLinkage, "barrier", Module);
    Builder.SetInsertPoint(
        llvm::BasicBlock::Create(Context, "entry", Function));
    Builder.CreateCall(llvm::InlineAsm::get(Type, "dsb sy", "", true));
    Builder.CreateRetVoid();
    CEmitterOptions Options;
    Options.TheArch = Architecture;
    std::string Source;
    llvm::raw_string_ostream Stream(Source);
    ASSERT_TRUE(LLVMCEmitter().emit(Module, Stream, Options));
    EXPECT_NE(Source.find("#include <arm_acle.h>"), std::string::npos)
        << Source;
    EXPECT_NE(Source.find("__dsb(0xF)"), std::string::npos) << Source;
    EXPECT_EQ(Source.find("#include <intrin.h>"), std::string::npos);
  }
}

} // namespace
