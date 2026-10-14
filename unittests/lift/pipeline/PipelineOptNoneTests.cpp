//===- PipelineOptNoneTests.cpp - Preserve native runtime contracts
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Common.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

using namespace neverd;

namespace {

llvm::Function *buildRoot(llvm::Module &M, llvm::StringRef Name, bool OptNone) {
  auto &C = M.getContext();
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *FT = llvm::FunctionType::get(I32, {I32}, false);
  auto *F =
      llvm::Function::Create(FT, llvm::Function::ExternalLinkage, Name, M);
  if (OptNone) {
    F->addFnAttr(llvm::Attribute::NoInline);
    F->addFnAttr(llvm::Attribute::OptimizeNone);
  }
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(C, "entry", F));
  auto *Slot = B.CreateAlloca(I32);
  auto *Store = B.CreateStore(F->getArg(0), Slot);
  Store->setMetadata(windows_eh_md::RegistrationRootAttachment,
                     llvm::MDNode::get(C, llvm::MDString::get(C, "root")));
  B.CreateRet(B.CreateLoad(I32, Slot));
  return F;
}

std::string body(const llvm::Function &F) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  for (const llvm::BasicBlock &BB : F)
    BB.print(OS);
  return Text;
}

bool hasAlloca(const llvm::Function &F) {
  for (const llvm::Instruction &I : llvm::instructions(F))
    if (llvm::isa<llvm::AllocaInst>(I))
      return true;
  return false;
}

TEST(PipelineOptNone, PromotionPreservesRuntimeRootStores) {
  llvm::LLVMContext C;
  llvm::Module M("roots", C);
  const std::string Before = body(*buildRoot(M, "callback", true));
  buildRoot(M, "ordinary", false);
  ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));

  Pipeline::promoteScaffoldingAllocas(M);

  EXPECT_EQ(body(*M.getFunction("callback")), Before);
  EXPECT_FALSE(hasAlloca(*M.getFunction("ordinary")));
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
}

TEST(PipelineOptNone, OptimizationPreservesRuntimeRootStores) {
  for (auto Strength :
       {Pipeline::OptStrength::Thin, Pipeline::OptStrength::Deep,
        Pipeline::OptStrength::Source}) {
    for (bool Conservative : {false, true}) {
      SCOPED_TRACE(static_cast<int>(Strength));
      SCOPED_TRACE(Conservative);
      llvm::LLVMContext C;
      llvm::Module M("roots", C);
      const std::string Before = body(*buildRoot(M, "callback", true));
      buildRoot(M, "ordinary", false);
      Pipeline::OptimizationOptions Options;
      Options.Strength = Strength;
      Options.Conservative = Conservative;

      const auto Result = Pipeline::optimizeModule(M, Options);

      EXPECT_NE(Result.Stop, OptimizationStopReason::InputInvalid);
      EXPECT_NE(Result.Stop, OptimizationStopReason::VerificationFailed);
      EXPECT_EQ(body(*M.getFunction("callback")), Before);
      EXPECT_FALSE(hasAlloca(*M.getFunction("ordinary")));
      EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
    }
  }
}

TEST(PipelineOptNone, ManualFrameCanonicalizationAlsoPreservesOptNone) {
  llvm::LLVMContext C;
  llvm::Module M("frame", C);
  M.setTargetTriple(llvm::Triple("i686-pc-windows-msvc"));
  M.setDataLayout("e-p:32:32");
  auto *F = buildRoot(M, "callback", true);
  auto &Entry = F->getEntryBlock();
  Entry.setName(kFrameSetupBlock);
  llvm::IRBuilder<> B(Entry.getTerminator());
  auto *Frame =
      B.CreateAlloca(llvm::ArrayType::get(B.getInt8Ty(), 64), nullptr, "frame");
  auto *End = B.CreateGEP(B.getInt8Ty(), Frame, B.getInt32(64), "frame_end");
  auto *SP = B.CreatePtrToInt(End, B.getInt32Ty(), kRspInitValue);
  auto *Pointer =
      B.CreateIntToPtr(B.CreateSub(SP, B.getInt32(8)), B.getPtrTy());
  B.CreateStore(F->getArg(0), Pointer);
  // The stack-pointer repair follows constant inttoptr accesses separately.
  auto *Negative =
      llvm::ConstantExpr::getIntToPtr(B.getInt32(-4), B.getPtrTy());
  B.CreateStore(B.getInt32(5), Negative);
  const std::string Before = body(*F);

  const auto Result =
      Pipeline::optimizeModule(M, Pipeline::OptimizationOptions{});

  EXPECT_NE(Result.Stop, OptimizationStopReason::VerificationFailed);
  EXPECT_EQ(body(*M.getFunction("callback")), Before);
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
}

} // namespace
