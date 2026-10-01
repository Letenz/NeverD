//===- InterpreterLLVMRefinementCompileTests.cpp - Exact compiled IR -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../lift/NeverDLiftFixture.h"
#include "InterpreterLLVMRefinementTest.h"

namespace neverd::analysis::llvm_refinement_test {
class CompiledLLVMRefinement : public NeverDLiftTest {};
TEST_F(CompiledLLVMRefinement, IndependentCArtifactMatchesActualNativeBytes) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  // lea rax,[rax+rcx*2+5]; ret. The independent C fixture below observes the
  // full state ABI, including unchanged registers, flags and status.
  Program P({0x48, 0x8d, 0x44, 0x48, 5, 0xc3});
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  const auto C = tmpFile("state.c"), IR = tmpFile("state.ll");
  std::ofstream(C) << R"(
    typedef unsigned long long word;
    word model(word *state) {
      state[0] = state[0] + state[1] * 2 + 5;
      return 0;
    }
  )";
  for (const char *Optimization : {"-O1", "-O2"}) {
    SCOPED_TRACE(Optimization);
    const auto Built =
        exec(NEVERD_TEST_CLANG,
             {"-target", "x86_64-unknown-linux-gnu", "-std=c11", Optimization,
              "-fno-vectorize", "-fno-slp-vectorize", "-S", "-emit-llvm",
              C.string(), "-o", IR.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    std::ifstream Input(IR, std::ios::binary);
    const std::string Text{std::istreambuf_iterator<char>(Input),
                           std::istreambuf_iterator<char>()};
    const auto Proof = P.check(R.Residual, Text);
    EXPECT_TRUE(Proof.proved()) << Proof.Diagnostic;
  }
}
} // namespace neverd::analysis::llvm_refinement_test
