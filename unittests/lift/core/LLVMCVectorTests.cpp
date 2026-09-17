//===- LLVMCVectorTests.cpp - Executable LLVM C vector semantics ----------===//
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

#include <stdexcept>

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
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-llvmc-vector", "c",
                                                  SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-llvmc-vector", "exe",
                                                  BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-llvmc-vector", "err",
                                                  ErrorPath));
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

TEST(LLVMCValues, IntegerVectorsAreScalarizedWithoutMutatingTheInputModule) {
  llvm::LLVMContext Context;
  llvm::Module Module("integer-vector-projection", Context);
  Module.setDataLayout("e-p:64:64");
  auto *I64 = llvm::Type::getInt64Ty(Context);
  auto *Vector = llvm::FixedVectorType::get(llvm::Type::getInt8Ty(Context), 4);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(I64, {I64}, false),
      llvm::GlobalValue::ExternalLinkage, "packed", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  llvm::Value *Lanes = llvm::PoisonValue::get(Vector);
  for (unsigned Lane = 0; Lane < 4; ++Lane)
    Lanes = Builder.CreateInsertElement(
        Lanes,
        Builder.CreateTrunc(Builder.CreateLShr(Function->getArg(0), Lane * 8),
                            Builder.getInt8Ty()),
        Lane);
  auto *Mask =
      Builder.CreateICmpNE(Lanes, llvm::Constant::getNullValue(Vector));
  Builder.CreateRet(Builder.CreateOr(
      Builder.CreateZExt(Builder.CreateBitCast(Mask, Builder.getIntNTy(4)),
                         I64),
      Builder.CreateZExt(Builder.CreateOrReduce(Lanes), I64)));

  std::string Before, After, Source;
  llvm::raw_string_ostream BeforeOut(Before), AfterOut(After), Out(Source);
  Module.print(BeforeOut, nullptr);
  ASSERT_TRUE(
      neverd::LLVMCEmitter().emit(Module, Out, {}, nullptr, nullptr, Function));
  Module.print(AfterOut, nullptr);
  EXPECT_EQ(Before, After);
  compileAndRun(Source + R"(
static uint64_t expected(uint64_t value) {
  uint64_t mask = 0, reduced = 0;
  for (unsigned lane = 0; lane < 4; ++lane) {
    uint64_t byte = (value >> (lane * 8)) & 0xff;
    if (byte) mask |= UINT64_C(1) << lane;
    reduced |= byte;
  }
  return mask | reduced;
}
int main(void) {
  const uint64_t values[] = {
      0, 1, UINT64_C(0x01020304), UINT64_C(0xff000000),
      UINT64_C(0x1020304050607080), UINT64_MAX};
  for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
    if (packed(values[i]) != expected(values[i])) return (int)i + 1;
  return 0;
}
)");
}

TEST(LLVMCValues, UnsupportedVectorPackingIsRejected) {
  llvm::LLVMContext Context;
  llvm::Module Module("float-vector-packing", Context);
  Module.setDataLayout("e-p:64:64");
  auto *I64 = llvm::Type::getInt64Ty(Context);
  auto *Vector = llvm::FixedVectorType::get(llvm::Type::getFloatTy(Context), 2);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(I64, {Vector}, false),
      llvm::GlobalValue::ExternalLinkage, "unsupported", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  Builder.CreateRet(Builder.CreateBitCast(Function->getArg(0), I64));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  EXPECT_THROW(neverd::LLVMCEmitter().emit(Module, Out, {}),
               std::runtime_error);
}

} // namespace
