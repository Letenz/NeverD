//===- LLVMScalarCompileTests.cpp - Independently compiled loop inputs ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../lift/NeverDLiftFixture.h"
#include "LLVMScalarEquivalenceTest.h"

#include "llvm/IRReader/IRReader.h"

namespace neverd::analysis::scalar_test {
class LLVMScalarCompiled : public NeverDLiftTest {};

TEST_F(LLVMScalarCompiled, NestedLoopAndFormulaAgreeForAllScalarInputs) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  const char *Programs[] = {
      R"(unsigned f(unsigned x, unsigned n) {
           unsigned value = x;
           for (unsigned a = 0; a < (n & 3u); ++a)
             for (unsigned b = 0; b < ((n >> 12) & 1u); ++b)
               value += 11u;
           return value;
         })",
      R"(unsigned f(unsigned x, unsigned n) {
           return x + 11u * (n & 3u) * ((n >> 12) & 1u);
         })"};
  for (const char *Optimization : {"-O1", "-O2"}) {
    SCOPED_TRACE(Optimization);
    llvm::LLVMContext Contexts[2];
    std::unique_ptr<llvm::Module> Modules[2];
    for (unsigned I = 0; I < 2; ++I) {
      auto C = tmpFile("source" + std::to_string(I) + ".c");
      auto IR = tmpFile("source" + std::to_string(I) + ".ll");
      std::ofstream(C) << Programs[I];
      auto Built =
          exec(NEVERD_TEST_CLANG,
               {"-target", "x86_64-unknown-linux-gnu", "-std=c11", Optimization,
                "-fno-vectorize", "-fno-slp-vectorize", "-fno-unroll-loops",
                "-S", "-emit-llvm", C.string(), "-o", IR.string()});
      ASSERT_TRUE(Built.ok()) << Built.err;
      llvm::SMDiagnostic Error;
      Modules[I] = llvm::parseIRFile(IR.string(), Error, Contexts[I]);
      ASSERT_TRUE(Modules[I]);
    }
    auto R = checkLLVMScalarEquivalence(*Modules[0]->getFunction("f"),
                                        *Modules[1]->getFunction("f"));
    EXPECT_EQ(R.Status, Status::Proved) << R.Diagnostic;
  }
}
} // namespace neverd::analysis::scalar_test
