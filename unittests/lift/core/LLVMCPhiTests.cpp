//===- LLVMCPhiTests.cpp - Executable LLVM C PHI semantics ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

#include <utility>

namespace {

void compileAndRun(const std::string &Source) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  auto Program = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Program)) << "clang is required for emitted C execution";
  const std::string Compiler = *Program;
#endif
  llvm::SmallString<128> SourcePath, BinaryPath, ErrorPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-llvmc-phi", "c", SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-llvmc-phi", "exe",
                                                  BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-llvmc-phi", "err", ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code EC;
  {
    llvm::raw_fd_ostream OS(SourcePath, EC);
    ASSERT_FALSE(EC);
    OS << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  const llvm::SmallVector<llvm::StringRef, 12> Arguments{
      Compiler,
      "-std=c11",
      "-O2",
      "-Werror=uninitialized",
      "-Werror=return-type",
      SourcePath,
      "-o",
      BinaryPath};
  std::string Error;
  int Result = llvm::sys::ExecuteAndWait(Compiler, Arguments, std::nullopt,
                                         Redirects, 30, 0, &Error);
  auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
  ASSERT_EQ(Result, 0) << Error << (Errors ? (*Errors)->getBuffer().str() : "")
                       << '\n'
                       << Source;
  Result = llvm::sys::ExecuteAndWait(BinaryPath, {BinaryPath}, std::nullopt,
                                     Redirects, 30, 0, &Error);
  ASSERT_EQ(Result, 0) << Error << '\n' << Source;
}

TEST(LLVMCValues, LoopPhiCopiesUseTheTakenEdgeAndPreserveParallelAssignments) {
  llvm::LLVMContext Context;
  llvm::Module Module("loop-phi-copies", Context);
  auto *I64 = llvm::Type::getInt64Ty(Context);
  auto *Pointer = llvm::PointerType::getUnqual(Context);
  auto *Signature = llvm::FunctionType::get(llvm::Type::getVoidTy(Context),
                                            {I64, Pointer, Pointer}, false);
  auto *Function = llvm::Function::Create(
      Signature, llvm::GlobalValue::ExternalLinkage, "swap_loop", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
  auto *Loop = llvm::BasicBlock::Create(Context, "loop", Function);
  auto *Body = llvm::BasicBlock::Create(Context, "body", Function);
  auto *Exit = llvm::BasicBlock::Create(Context, "exit", Function);
  llvm::IRBuilder<> Builder(Entry);
  Builder.CreateBr(Loop);
  Builder.SetInsertPoint(Loop);
  auto *First = Builder.CreatePHI(I64, 2, "first");
  auto *Second = Builder.CreatePHI(I64, 2, "second");
  auto *Index = Builder.CreatePHI(I64, 2, "index");
  First->addIncoming(Builder.getInt64(11), Entry);
  Second->addIncoming(Builder.getInt64(29), Entry);
  Index->addIncoming(Builder.getInt64(0), Entry);
  Builder.CreateCondBr(Builder.CreateICmpULT(Index, Function->getArg(0)), Body,
                       Exit);
  Builder.SetInsertPoint(Body);
  auto *Next = Builder.CreateAdd(Index, Builder.getInt64(1));
  Builder.CreateBr(Loop);
  First->addIncoming(Second, Body);
  Second->addIncoming(First, Body);
  Index->addIncoming(Next, Body);
  Builder.SetInsertPoint(Exit);
  Builder.CreateStore(First, Function->getArg(1));
  Builder.CreateStore(Second, Function->getArg(2));
  Builder.CreateRetVoid();
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  compileAndRun(Source + R"(
int main(void) {
  for (uint64_t count = 0; count < 32; ++count) {
    uint64_t first = 0, second = 0;
    swap_loop(count, &first, &second);
    if (first != (count & 1 ? 29 : 11)) return 1;
    if (second != (count & 1 ? 11 : 29)) return 2;
  }
  return 0;
}
)");
}

TEST(LLVMCValues, InlinedFalseArmKeepsLoopEntryPhiCopy) {
  llvm::LLVMContext Context;
  llvm::Module Module("inlined-loop-entry", Context);
  auto *I64 = llvm::Type::getInt64Ty(Context);
  auto *Pointer = llvm::PointerType::getUnqual(Context);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(I64, {Pointer}, false),
      llvm::GlobalValue::ExternalLinkage, "seeded_loop", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
  auto *First = llvm::BasicBlock::Create(Context, "first", Function);
  auto *Loop = llvm::BasicBlock::Create(Context, "loop", Function);
  auto *Body = llvm::BasicBlock::Create(Context, "body", Function);
  // This preheader is physically after the loop but is printed on the false
  // edge of First. Its outgoing edge still initializes the shared PHI.
  auto *Preheader = llvm::BasicBlock::Create(Context, "preheader", Function);
  auto *Exit = llvm::BasicBlock::Create(Context, "exit", Function);
  llvm::IRBuilder<> B(Entry);
  auto *State = Function->getArg(0);
  auto *Count = B.CreateLoad(I64, State);
  B.CreateCondBr(B.CreateICmpEQ(Count, B.getInt64(0)), Exit, First);
  B.SetInsertPoint(First);
  B.CreateStore(B.CreateSub(Count, B.getInt64(1)), State);
  auto *Value = B.CreateGEP(I64, State, B.getInt64(1));
  auto Update = [&](uint64_t Addend) {
    return B.CreateAdd(B.CreateMul(B.CreateLoad(I64, Value), B.getInt64(13)),
                       B.getInt64(Addend));
  };
  B.CreateStore(Update(222), Value);
  auto *Remaining = B.CreateLoad(I64, State);
  B.CreateCondBr(B.CreateICmpEQ(Remaining, B.getInt64(0)), Exit, Preheader);
  B.SetInsertPoint(Loop);
  auto *Merged = B.CreatePHI(I64, 2, "merged");
  B.CreateStore(Merged, Value);
  auto *Budget = B.CreateLoad(I64, State);
  B.CreateCondBr(B.CreateICmpEQ(Budget, B.getInt64(0)), Exit, Body);
  B.SetInsertPoint(Body);
  B.CreateStore(B.CreateSub(Budget, B.getInt64(1)), State);
  Merged->addIncoming(Update(62), Body);
  B.CreateBr(Loop);
  B.SetInsertPoint(Preheader);
  B.CreateStore(B.CreateSub(Remaining, B.getInt64(1)), State);
  Merged->addIncoming(Update(215), Preheader);
  B.CreateBr(Loop);
  B.SetInsertPoint(Exit);
  B.CreateRet(B.getInt64(0));
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, Options));
  compileAndRun(Source + R"(
int main(void) {
  for (uint64_t count = 0; count < 32; ++count) {
    for (uint64_t seed = 0; seed < 64; ++seed) {
      uint64_t state[2] = {count, seed}, expected = seed;
      for (uint64_t step = 0; step < count; ++step)
        expected = expected * 13 + (step == 0 ? 222 : step == 1 ? 215 : 62);
      if (seeded_loop(state) || state[0] || state[1] != expected) return 1;
    }
  }
  return 0;
}
)");
}

TEST(LLVMCValues, BranchAndSwitchPhiCopiesFollowSelectedEdges) {
  llvm::LLVMContext Context;
  llvm::Module Module("branch-switch-phi-copies", Context);
  auto *I64 = llvm::Type::getInt64Ty(Context);
  auto *Signature = llvm::FunctionType::get(I64, {I64}, false);

  auto *Diamond = llvm::Function::Create(
      Signature, llvm::GlobalValue::ExternalLinkage, "diamond", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", Diamond);
  auto *Yes = llvm::BasicBlock::Create(Context, "yes", Diamond);
  auto *No = llvm::BasicBlock::Create(Context, "no", Diamond);
  auto *Join = llvm::BasicBlock::Create(Context, "join", Diamond);
  llvm::IRBuilder<> Builder(Entry);
  Builder.CreateCondBr(
      Builder.CreateICmpEQ(Diamond->getArg(0), Builder.getInt64(0)), Yes, No);
  Builder.SetInsertPoint(Yes);
  Builder.CreateBr(Join);
  Builder.SetInsertPoint(No);
  Builder.CreateBr(Join);
  Builder.SetInsertPoint(Join);
  auto *Answer = Builder.CreatePHI(I64, 2, "answer");
  Answer->addIncoming(Builder.getInt64(11), Yes);
  Answer->addIncoming(Builder.getInt64(22), No);
  auto *Unused = Builder.CreatePHI(I64, 2, "unused");
  Unused->addIncoming(Builder.getInt64(33), Yes);
  Unused->addIncoming(Builder.getInt64(44), No);
  Builder.CreateRet(Answer);

  auto *Switch = llvm::Function::Create(
      Signature, llvm::GlobalValue::ExternalLinkage, "switch_phi", Module);
  Entry = llvm::BasicBlock::Create(Context, "entry", Switch);
  auto *Zero = llvm::BasicBlock::Create(Context, "zero", Switch);
  auto *One = llvm::BasicBlock::Create(Context, "one", Switch);
  auto *Other = llvm::BasicBlock::Create(Context, "other", Switch);
  Builder.SetInsertPoint(Entry);
  auto *Dispatch = Builder.CreateSwitch(Switch->getArg(0), Other, 2);
  Dispatch->addCase(Builder.getInt64(0), Zero);
  Dispatch->addCase(Builder.getInt64(1), One);
  for (auto [Block, Value] :
       {std::pair{Zero, uint64_t(101)}, std::pair{One, uint64_t(202)},
        std::pair{Other, uint64_t(303)}}) {
    Builder.SetInsertPoint(Block);
    auto *Selected = Builder.CreatePHI(I64, 1, "selected");
    Selected->addIncoming(Builder.getInt64(Value), Entry);
    Builder.CreateRet(Selected);
  }

  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  compileAndRun(Source + R"(
int main(void) {
  if (diamond(0) != 11 || diamond(7) != 22) return 1;
  if (switch_phi(0) != 101 || switch_phi(1) != 202) return 2;
  if (switch_phi(2) != 303 || switch_phi(UINT64_MAX) != 303) return 3;
  return 0;
}
)");
}

} // namespace
