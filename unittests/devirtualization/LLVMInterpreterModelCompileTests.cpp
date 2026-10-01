//===- LLVMInterpreterModelCompileTests.cpp - Independent compiled input
//---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../lift/NeverDLiftFixture.h"
#include "LLVMInterpreterModelTest.h"

#include "llvm/IRReader/IRReader.h"

namespace neverd::analysis::llvm_model_test {
class LLVMCompiledModel : public NeverDLiftTest {};
TEST_F(LLVMCompiledModel, CompiledScalarCMatchesIndependentFullStateOracle) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  auto Source = tmpFile("scalar.c"), IR = tmpFile("scalar.ll");
  std::ofstream(Source) << R"(
    typedef unsigned long long word;
    word model(unsigned char *s) {
      word a = *(word *)(s + 8), b = *(word *)(s + 16);
      word value = ((a ^ 37) + b) ^ *(word *)s;
      *(word *)s = value;
      *(word *)(s + 24) ^= value;
      return 0;
    }
  )";
  Oracle Reference({op(NdOp::INT_XOR, r(256), {r(8), n(37)}),
                    op(NdOp::INT_ADD, r(264), {r(256), r(16)}),
                    op(NdOp::INT_XOR, r(0), {r(264), r(0)}),
                    op(NdOp::INT_XOR, r(24), {r(24), r(0)})});
  for (const char *Optimization : {"-O1", "-O2"}) {
    SCOPED_TRACE(Optimization);
    auto Built = exec(NEVERD_TEST_CLANG,
                      {"-target", "x86_64-unknown-linux-gnu", "-std=c11",
                       Optimization, "-fno-vectorize", "-fno-slp-vectorize",
                       "-S", "-emit-llvm", Source.string(), "-o", IR.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    llvm::LLVMContext Context;
    llvm::SMDiagnostic Error;
    auto Module = llvm::parseIRFile(IR.string(), Error, Context);
    ASSERT_TRUE(Module) << "Configured Clang produced unreadable LLVM IR";
    auto *Function = Module->getFunction("model");
    ASSERT_NE(Function, nullptr);
    auto M = modelLLVMInterpreterMachineStateX64(*Function);
    ASSERT_TRUE(static_cast<bool>(M)) << llvm::toString(M.takeError());
    auto R =
        checkLowIRRefinement(M->Function, M->Instructions, Reference.Function,
                             llvmInterpreterMachineStateContract(),
                             LowIRRefinementWitness::LiftedBits);
    EXPECT_TRUE(R.proved()) << R.Diagnostic;
  }
}

TEST_F(LLVMCompiledModel, CompiledVariableShiftMatchesFullStateOracle) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  auto Source = tmpFile("shift.c"), IR = tmpFile("shift.ll");
  std::ofstream(Source) << R"(
    typedef unsigned long long word;
    word model(unsigned char *s) {
      word value = *(word *)s, count = *(word *)(s + 8) & 63;
      *(word *)s = value ^ (value >> count);
      return 0;
    }
  )";
  Oracle Reference({op(NdOp::INT_AND, r(256), {r(8), n(63)}),
                    op(NdOp::INT_RIGHT, r(264), {r(0), r(256)}),
                    op(NdOp::INT_XOR, r(0), {r(0), r(264)})});
  for (const char *Optimization : {"-O1", "-O2"}) {
    SCOPED_TRACE(Optimization);
    auto Built = exec(NEVERD_TEST_CLANG,
                      {"-target", "x86_64-unknown-linux-gnu", "-std=c11",
                       Optimization, "-fno-vectorize", "-fno-slp-vectorize",
                       "-S", "-emit-llvm", Source.string(), "-o", IR.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    llvm::LLVMContext Context;
    llvm::SMDiagnostic Error;
    auto Module = llvm::parseIRFile(IR.string(), Error, Context);
    ASSERT_TRUE(Module);
    auto *Function = Module->getFunction("model");
    ASSERT_NE(Function, nullptr);
    auto M = modelLLVMInterpreterMachineStateX64(*Function);
    ASSERT_TRUE(static_cast<bool>(M)) << llvm::toString(M.takeError());
    auto R =
        checkLowIRRefinement(M->Function, M->Instructions, Reference.Function,
                             llvmInterpreterMachineStateContract(),
                             LowIRRefinementWitness::LiftedBits);
    EXPECT_TRUE(R.proved()) << R.Diagnostic;
  }
}
} // namespace neverd::analysis::llvm_model_test
