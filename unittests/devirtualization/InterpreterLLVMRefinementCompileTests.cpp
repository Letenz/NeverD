//===- InterpreterLLVMRefinementCompileTests.cpp - Exact compiled IR -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../lift/NeverDLiftFixture.h"
#include "InterpreterLLVMRefinementTest.h"

namespace neverd::analysis::llvm_refinement_test {
class CompiledLLVMRefinement : public NeverDLiftTest {};

TEST_F(CompiledLLVMRefinement, PhysicalReturnsBindCompiledFrameWrites) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  // CALL helper; two invalid bytes; MOV EAX,7; RET.
  // helper: LEA RAX,[continuation]; MOV [RSP],RAX; RET. No flag changes.
  Program P({0xe8, 8,    0,    0,    0,    0x16, 0x06, 0xb8, 7,
             0,    0,    0,    0xc3, 0x48, 0x8d, 0x05, 0xf3, 0xff,
             0xff, 0xff, 0x48, 0x89, 0x04, 0x24, 0xc3});
  P.Options.EntryFrameBounds =
      SpecializationEntryFrameBounds{P.Frame.Begin, P.Frame.End};
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  const auto C = tmpFile("return-state.c"), IR = tmpFile("return-state.ll");
  for (bool WrongFrame : {false, true}) {
    std::ofstream(C) << "typedef unsigned long long word;\n"
                        "word model(word *state) {\n"
                        " word slot = "
                     << (WrongFrame ? 0x1006 : 0x1007)
                     << ";\n"
                        " __builtin_memcpy((void *)(state[4] - 8), &slot, 8);\n"
                        " state[0] = 7;\n"
                        " return 0;\n}\n";
    for (const char *Optimization : {"-O1", "-O2"}) {
      SCOPED_TRACE(WrongFrame);
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
      ASSERT_TRUE(Proof.Native.proved())
          << Proof.Diagnostic << "\n"
          << Proof.Native.Proof.Diagnostic << "\n"
          << Text;
      if (WrongFrame) {
        rejected(Proof, Stage::LLVM);
        EXPECT_EQ(Proof.LLVM.Status, Status::Different);
      } else {
        EXPECT_TRUE(Proof.proved()) << Proof.Diagnostic;
      }
    }
  }
}

TEST_F(CompiledLLVMRefinement, EntryAlignmentValidatesExactCompiledC) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  // lea rax,[rsp+rcx*2+5]; ret. The guard is authored independently of the
  // native recovery. Its rejected-domain status must not become a premise.
  Program P({0x48, 0x8d, 0x44, 0x4c, 5, 0xc3});
  const auto C = tmpFile("aligned-state.c"), IR = tmpFile("aligned-state.ll");
  for (uint32_t Residue : {3, 8}) {
    P.Options.EntryFrameAlignment = P.Frame.EntryAlignment =
        InterpreterEntryAlignment{16, Residue};
    const auto R = P.recover();
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    std::ofstream(C) << "typedef unsigned long long word;\n"
                        "word model(word *state) {\n"
                        " if ((state[4] & 15) != "
                     << Residue
                     << ") return 2;\n"
                        " state[0] = state[4] + state[1] * 2 + 5;\n"
                        " return 0;\n}\n";
    for (const char *Optimization : {"-O1", "-O2"}) {
      SCOPED_TRACE(Residue);
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
      ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
      P.Options.EntryFrameAlignment->Residue = Residue ^ 1;
      P.Frame.EntryAlignment = P.Options.EntryFrameAlignment;
      const auto WrongDomain = P.check(R.Residual, Text);
      rejected(WrongDomain, Stage::LLVM);
      EXPECT_TRUE(WrongDomain.Native.proved());
      EXPECT_EQ(WrongDomain.LLVM.Status, Status::Different);
      P.Options.EntryFrameAlignment->Residue = Residue;
      P.Frame.EntryAlignment = P.Options.EntryFrameAlignment;
    }
  }
}

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

TEST_F(CompiledLLVMRefinement, OutputOnlyStateWordRetainsCompilerContracts) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  // lea rax,[rcx+rdx*2+5]; ret. Clang can attach initializes((0,8))
  // because the first word is written before any read. Keep the exact IR.
  Program P({0x48, 0x8d, 0x44, 0x51, 5, 0xc3});
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  const auto C = tmpFile("output.c"), IR = tmpFile("output.ll");
  std::ofstream(C) << R"(
    typedef unsigned long long word;
    word model(word *state) {
      state[0] = state[1] + state[2] * 2 + 5;
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
