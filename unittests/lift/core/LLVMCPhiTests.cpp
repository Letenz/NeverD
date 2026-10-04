//===- LLVMCPhiTests.cpp - Executable LLVM C PHI semantics ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"

#include <utility>

namespace {

void compileAndRun(const std::string &Source,
                   llvm::StringRef Optimization = "-O2") {
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
      Optimization,
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

TEST(LLVMCValues, FoldedStoreArmsPublishTheirOutgoingPhiValues) {
  llvm::LLVMContext Context;
  llvm::Module Module("store-arm-phi", Context);
  llvm::IRBuilder<> B(Context);
  auto *I64 = B.getInt64Ty();
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy(), B.getPtrTy(), I64},
                              false),
      llvm::GlobalValue::ExternalLinkage, "choose_and_store", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
  auto *Left = llvm::BasicBlock::Create(Context, "left", Function);
  auto *Right = llvm::BasicBlock::Create(Context, "right", Function);
  auto *Join = llvm::BasicBlock::Create(Context, "join", Function);
  B.SetInsertPoint(Entry);
  B.CreateCondBr(B.CreateICmpNE(Function->getArg(2), B.getInt64(0)), Left,
                 Right);
  B.SetInsertPoint(Left);
  auto *A = B.CreateAdd(B.CreateLoad(I64, Function->getArg(0)), B.getInt64(7));
  B.CreateStore(A, Function->getArg(0));
  auto *LeftValue = B.CreateXor(A, B.getInt64(0x55));
  B.CreateBr(Join);
  B.SetInsertPoint(Right);
  auto *C = B.CreateMul(B.CreateLoad(I64, Function->getArg(0)), B.getInt64(3));
  B.CreateStore(C, Function->getArg(0));
  auto *RightValue = B.CreateAdd(C, B.getInt64(0x33));
  B.CreateBr(Join);
  B.SetInsertPoint(Join);
  auto *Merged = B.CreatePHI(I64, 2, "merged");
  Merged->addIncoming(LeftValue, Left);
  Merged->addIncoming(RightValue, Right);
  B.CreateStore(Merged, Function->getArg(1));
  B.CreateRetVoid();
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Output(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Output, {}));
  Source += R"(
int main(void) {
  for (uint64_t seed = 0; seed < 256; ++seed)
    for (uint64_t flag = 0; flag < 2; ++flag) {
      uint64_t input = seed, output = 0;
      choose_and_store(&input, &output, flag);
      if (input != (flag ? seed + 7 : seed * 3)) return 1;
      if (output != (flag ? ((seed + 7) ^ 0x55) : seed * 3 + 0x33)) return 2;
    }
  return 0;
}
)";
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source, Optimization);
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

TEST(LLVMCValues, IndependentLoopPhiUpdatesNeedNoSnapshotLocals) {
  llvm::LLVMContext Context;
  llvm::Module Module("independent-loop-updates", Context);
  llvm::IRBuilder<> B(Context);
  auto *I64 = B.getInt64Ty();
  auto *F = llvm::Function::Create(
      llvm::FunctionType::get(B.getVoidTy(), {I64, I64, B.getPtrTy()}, false),
      llvm::GlobalValue::ExternalLinkage, "independent_loop", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", F);
  auto *Loop = llvm::BasicBlock::Create(Context, "loop", F);
  auto *Body = llvm::BasicBlock::Create(Context, "body", F);
  auto *Exit = llvm::BasicBlock::Create(Context, "exit", F);
  B.SetInsertPoint(Entry);
  B.CreateBr(Loop);
  B.SetInsertPoint(Loop);
  auto *A = B.CreatePHI(I64, 2, "value");
  auto *C = B.CreatePHI(I64, 2, "value_suffix");
  auto *Index = B.CreatePHI(I64, 2, "index");
  A->addIncoming(F->getArg(1), Entry);
  C->addIncoming(B.getInt64(11), Entry);
  Index->addIncoming(B.getInt64(0), Entry);
  B.CreateCondBr(B.CreateICmpULT(Index, F->getArg(0)), Body, Exit);
  B.SetInsertPoint(Body);
  A->addIncoming(B.CreateAdd(A, B.getInt64(3)), Body);
  C->addIncoming(B.CreateAdd(B.CreateMul(C, B.getInt64(5)), B.getInt64(9)),
                 Body);
  Index->addIncoming(B.CreateAdd(Index, B.getInt64(1)), Body);
  B.CreateBr(Loop);
  B.SetInsertPoint(Exit);
  B.CreateStore(A, F->getArg(2));
  B.CreateStore(C, B.CreateGEP(I64, F->getArg(2), B.getInt64(1)));
  B.CreateRetVoid();
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, Options));
  EXPECT_EQ(Source.find("phi_edge"), std::string::npos) << Source;
  Source += R"(
int main(void) {
  uint64_t seed = UINT64_MAX;
  for (unsigned trial = 0; trial < 128; ++trial) {
    seed = seed * UINT64_C(6364136223846793005) + 1;
    for (uint64_t count = 0; count < 33; ++count) {
      uint64_t output[2] = {0, 0}, a = seed, b = 11;
      for (uint64_t i = 0; i < count; ++i) { a += 3; b = b * 5 + 9; }
      independent_loop(count, seed, output);
      if (output[0] != a || output[1] != b) return 1;
    }
  }
  return 0;
}
)";
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source, Optimization);
}

TEST(LLVMCValues, CompoundLoopPhiReadsKeepOnlyNeededSnapshots) {
  llvm::LLVMContext Context;
  llvm::Module Module("compound-loop-updates", Context);
  llvm::IRBuilder<> B(Context);
  auto *I64 = B.getInt64Ty();
  auto *F = llvm::Function::Create(
      llvm::FunctionType::get(B.getVoidTy(), {I64, I64, B.getPtrTy()}, false),
      llvm::GlobalValue::ExternalLinkage, "compound_loop", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", F);
  auto *Loop = llvm::BasicBlock::Create(Context, "loop", F);
  auto *Body = llvm::BasicBlock::Create(Context, "body", F);
  auto *Exit = llvm::BasicBlock::Create(Context, "exit", F);
  B.SetInsertPoint(Entry);
  B.CreateBr(Loop);
  B.SetInsertPoint(Loop);
  auto *A = B.CreatePHI(I64, 2, "first");
  auto *C = B.CreatePHI(I64, 2, "second");
  auto *D = B.CreatePHI(I64, 2, "third");
  auto *Index = B.CreatePHI(I64, 2, "index");
  A->addIncoming(F->getArg(1), Entry);
  C->addIncoming(B.getInt64(13), Entry);
  D->addIncoming(B.getInt64(29), Entry);
  Index->addIncoming(B.getInt64(0), Entry);
  B.CreateCondBr(B.CreateICmpULT(Index, F->getArg(0)), Body, Exit);
  B.SetInsertPoint(Body);
  A->addIncoming(B.CreateAdd(C, B.getInt64(7)), Body);
  C->addIncoming(B.CreateXor(D, A), Body);
  D->addIncoming(B.CreateMul(B.CreateAdd(A, D), B.getInt64(3)), Body);
  Index->addIncoming(B.CreateAdd(Index, B.getInt64(1)), Body);
  B.CreateBr(Loop);
  B.SetInsertPoint(Exit);
  B.CreateStore(A, F->getArg(2));
  B.CreateStore(C, B.CreateGEP(I64, F->getArg(2), B.getInt64(1)));
  B.CreateStore(D, B.CreateGEP(I64, F->getArg(2), B.getInt64(2)));
  B.CreateRetVoid();
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, Options));
  // One snapshot is read once; acyclic updates and loop seeds use assignments.
  EXPECT_EQ(llvm::StringRef(Source).count("phi_edge"), 2U) << Source;
  Source += R"(
int main(void) {
  uint64_t seed = 0;
  for (unsigned trial = 0; trial < 128; ++trial) {
    seed = seed * UINT64_C(2862933555777941757) + UINT64_C(3037000493);
    for (uint64_t count = 0; count < 33; ++count) {
      uint64_t output[3] = {0, 0, 0}, a = seed, b = 13, c = 29;
      for (uint64_t i = 0; i < count; ++i) {
        uint64_t next_a = b + 7, next_b = c ^ a, next_c = (a + c) * 3;
        a = next_a; b = next_b; c = next_c;
      }
      compound_loop(count, seed, output);
      if (output[0] != a || output[1] != b || output[2] != c) return 1;
    }
  }
  return 0;
}
)";
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source, Optimization);
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

TEST(LLVMCValues, MovedFalseArmCannotRunAfterAnInlinedTrueArm) {
  llvm::LLVMContext Context;
  llvm::Module Module("moved-false-arm", Context);
  llvm::IRBuilder<> B(Context);
  auto *I64 = B.getInt64Ty();
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(I64, {B.getPtrTy(), I64}, false),
      llvm::GlobalValue::ExternalLinkage, "choose_effect", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
  // Both arm bodies appear after the join in LLVM layout. Printing the false
  // body before the join must not make it a continuation of the true arm.
  auto *Join = llvm::BasicBlock::Create(Context, "join", Function);
  auto *Then = llvm::BasicBlock::Create(Context, "then", Function);
  auto *Else = llvm::BasicBlock::Create(Context, "else", Function);
  B.SetInsertPoint(Entry);
  auto *Seed = B.CreateLoad(I64, Function->getArg(0));
  B.CreateCondBr(B.CreateICmpNE(Function->getArg(1), B.getInt64(0)), Then,
                 Else);
  B.SetInsertPoint(Then);
  auto *Count = B.CreateUnaryIntrinsic(llvm::Intrinsic::ctpop, Seed);
  B.CreateStore(Count, Function->getArg(0));
  auto *TrueValue = B.CreateAdd(Count, B.getInt64(7));
  B.CreateBr(Join);
  B.SetInsertPoint(Else);
  auto *Product = B.CreateMul(Seed, B.getInt64(3));
  B.CreateStore(Product, Function->getArg(0));
  auto *FalseValue = B.CreateAdd(Product, B.getInt64(13));
  B.CreateBr(Join);
  B.SetInsertPoint(Join);
  auto *Result = B.CreatePHI(I64, 2, "result");
  Result->addIncoming(TrueValue, Then);
  Result->addIncoming(FalseValue, Else);
  B.CreateRet(Result);
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Output(Source);
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Output, Options));
  Source += R"(
int main(void) {
  for (uint64_t seed = 0; seed < 256; ++seed)
    for (uint64_t flag = 0; flag < 2; ++flag) {
      uint64_t value = seed;
      uint64_t expected = flag ? __builtin_popcountll(seed) : seed * 3;
      if (choose_effect(&value, flag) != expected + (flag ? 7 : 13)) return 1;
      if (value != expected) return 2;
    }
  return 0;
}
)";
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source, Optimization);
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

const char *CommonBranchIR = R"(
declare void @record_value(i32, ptr)
define i64 @branch_loop(i32 %seed, i32 %count, i32 %flag, ptr %output) {
entry:
  %limit = and i32 %count, 31
  %second = getelementptr i32, ptr %output, i32 1
  br label %head
head:
  %index = phi i32 [0, %entry], [%inc, %latch]
  %value = phi i32 [%seed, %entry], [%carried, %latch]
  %toggle = xor i32 %index, %flag
  %bit = and i32 %toggle, 1
  %choose = icmp ne i32 %bit, 0
  br i1 %choose, label %left, label %right
left:
  %a = add i32 %value, 7
  %aa = xor i32 %value, -1
  call void @record_value(i32 %a, ptr %output)
  call void @record_value(i32 %aa, ptr %second)
  %done.left = icmp uge i32 %index, %limit
  br i1 %done.left, label %exit, label %latch
right:
  %b = xor i32 %value, 53
  %bb = add i32 %value, 9
  call void @record_value(i32 %b, ptr %output)
  call void @record_value(i32 %bb, ptr %second)
  %done.right = icmp uge i32 %index, %limit
  br i1 %done.right, label %exit, label %latch
latch:
  %carried = phi i32 [%aa, %left], [%bb, %right]
  %inc = add i32 %index, 1
  br label %head
exit:
  %first = phi i32 [%a, %left], [%b, %right]
  %last = phi i32 [%aa, %left], [%bb, %right]
  %wide.first = zext i32 %first to i64
  %upper = shl i64 %wide.first, 32
  %wide.last = zext i32 %last to i64
  %result = or i64 %upper, %wide.last
  ret i64 %result
}
)";

void checkCommonBranch(bool DifferentCondition, bool AdditionalPredecessor,
                       bool OnlyFunction, bool SnapshotCondition = false) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Error;
  std::string IR = CommonBranchIR;
  if (SnapshotCondition) {
    const std::string Old = "%limit = and i32 %count, 31";
    IR.replace(IR.find(Old), Old.size(),
               "%entry.count = load i32, ptr %output\n"
               "  %limit = and i32 %entry.count, 31");
  }
  if (DifferentCondition) {
    const std::string Old = "%done.right = icmp uge i32 %index, %limit";
    IR.replace(IR.find(Old), Old.size(),
               "%other.limit = add i32 %limit, 1\n"
               "  %done.right = icmp uge i32 %index, %other.limit");
  }
  if (AdditionalPredecessor) {
    const std::string Old = "br i1 %choose, label %left, label %right\nleft:";
    IR.replace(IR.find(Old), Old.size(),
               "br i1 %choose, label %left, label %bridge\n"
               "bridge:\n"
               "  br i1 %choose, label %left, label %right\nleft:");
  }
  auto Module = llvm::parseAssemblyString(IR, Error, Context);
  ASSERT_TRUE(Module);
  ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  std::string Before;
  llvm::raw_string_ostream BeforeStream(Before);
  Module->print(BeforeStream, nullptr);
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  std::string Source;
  llvm::raw_string_ostream Output(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(
      *Module, Output, Options, nullptr, nullptr,
      OnlyFunction ? Module->getFunction("branch_loop") : nullptr));
  std::string After;
  llvm::raw_string_ostream AfterStream(After);
  Module->print(AfterStream, nullptr);
  EXPECT_EQ(Before, After) << "Source normalization must use its own clone";
  if (!DifferentCondition && !AdditionalPredecessor) {
    size_t Comparisons = 0;
    for (llvm::StringRef Token : {" >= ", " < "})
      for (size_t At = 0;
           (At = Source.find(Token.str(), At)) != std::string::npos;
           At += Token.size())
        ++Comparisons;
    EXPECT_EQ(Comparisons, 1U) << Source;
  }
  // The external observer is supplied by the harness, including when only a
  // selected function's source fragment is requested.
  Source =
      "#include <stdint.h>\nvoid record_value(uint32_t, void *);\n" + Source;
  Source += "\n#define DIFFERENT_CONDITION " +
            std::to_string(DifferentCondition) + "\n";
  Source += R"(
static volatile uint32_t observed_trace;
void record_value(uint32_t value, void *address) {
  *(uint32_t *)address = value;
  observed_trace = observed_trace * UINT32_C(65599) + value;
}
int main(void) {
  uint32_t seed = UINT32_MAX;
  for (unsigned trial = 0; trial < 128; ++trial) {
    seed = seed * UINT32_C(1664525) + UINT32_C(1013904223);
    for (uint32_t count = 0; count < 32; ++count)
      for (uint32_t flag = 0; flag < 4; ++flag) {
        uint32_t value = seed, first = 0, last = 0, trace = 0;
        for (uint32_t i = 0;; ++i) {
          uint32_t choose = (i ^ flag) & 1;
          first = choose ? value + 7 : value ^ 53;
          last = choose ? ~value : value + 9;
          trace = trace * UINT32_C(65599) + first;
          trace = trace * UINT32_C(65599) + last;
          if (i >= count + (DIFFERENT_CONDITION && !choose)) break;
          value = last;
        }
        uint32_t output[] = {count, 0, UINT32_C(0xa154e93b)};
        observed_trace = 0;
        uint64_t actual = branch_loop(seed, count, flag, (void *)output);
        if (actual != (((uint64_t)first << 32) | last)) return 1;
        if (output[0] != first || output[1] != last) return 2;
        if (output[2] != UINT32_C(0xa154e93b)) return 3;
        if (observed_trace != trace) return 4;
      }
  }
  return 0;
}
)";
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source, Optimization);
}

TEST(LLVMCValues, CommonLoopExitKeepsArmEffectsAndBothPhiPairs) {
  checkCommonBranch(false, false, false);
  checkCommonBranch(false, false, true);
}

TEST(LLVMCValues, DifferentExitComparisonsKeepTheirOwnControl) {
  checkCommonBranch(true, false, false);
}

TEST(LLVMCValues, CommonExitArmsWithAnotherPredecessorRemainValid) {
  checkCommonBranch(false, true, false);
}

TEST(LLVMCValues, CommonExitRetainsValuesReadBeforeArmCalls) {
  checkCommonBranch(false, false, false, true);
}

} // namespace
