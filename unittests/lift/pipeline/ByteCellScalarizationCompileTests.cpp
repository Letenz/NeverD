//===- ByteCellScalarizationCompileTests.cpp - Cell memory oracles --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../LLVMHostFixture.h"
#include "../NeverDLiftFixture.h"
#include "ByteCellScalarizationTest.h"

namespace neverd::cell_test {
class ByteCellCompiled : public NeverDLiftTest {};

TEST_F(ByteCellCompiled, CompleteMemoryAndReturnOracleAtO0AndO2) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  llvm::LLVMContext C;
  auto M = parse(C, program());
  ASSERT_TRUE(M);
  auto &F = *M->getFunction("f");
  F.setName("before");
  auto Original = tmpFile("before.ll");
  std::ofstream(Original) << neverd::test::printHostCompilerFixture(*M);
  ASSERT_EQ(ByteCellScalarizationPass::scalarize(F).Objects, 1u);
  F.setName("after");
  auto After = tmpFile("after.ll");
  std::ofstream(After) << neverd::test::printHostCompilerFixture(*M);
  Pipeline::OptimizationOptions Options;
  Options.Strength = Pipeline::OptStrength::Deep;
  auto R = Pipeline::optimizeModule(*M, Options);
  ASSERT_NE(R.Stop, OptimizationStopReason::VerificationFailed);
  M->getFunction("after")->setName("optimized");
  auto Optimized = tmpFile("optimized.ll");
  std::ofstream(Optimized) << neverd::test::printHostCompilerFixture(*M);
  auto Harness = tmpFile("oracle.c");
  std::ofstream(Harness) << R"(
#include <stdint.h>
#include <string.h>
extern uint64_t before(uint64_t, uint64_t, uint32_t, void *);
extern uint64_t after(uint64_t, uint64_t, uint32_t, void *);
extern uint64_t optimized(uint64_t, uint64_t, uint32_t, void *);
static uint64_t state = UINT64_C(0x189bab15da871f41);
static uint64_t word(void) {
  state ^= state << 13; state ^= state >> 7; state ^= state << 17;
  return state;
}
int main(void) {
  uint64_t (*functions[])(uint64_t,uint64_t,uint32_t,void*) =
      {before, after, optimized};
  for (unsigned k = 0; k != 8192; ++k) {
    uint64_t x = k < 256 ? k : word();
    uint64_t y = k < 256 ? ~(uint64_t)k : word();
    uint32_t n = k < 256 ? k : (uint32_t)word();
    unsigned char bytes[24];
    uint64_t seed = x ^ y;
    memcpy(bytes, &x, 8); memcpy(bytes + 8, &y, 8);
    memcpy(bytes + 16, &seed, 8);
    if (n & 1u) {
      uint32_t small = (uint32_t)x; memcpy(bytes + 1, &small, 4);
    } else {
      uint16_t small = (uint16_t)y; memcpy(bytes + 1, &small, 2);
    }
    for (uint32_t i = 0; i < (n & 15u); ++i) {
      uint64_t old; memcpy(&old, bytes + 3, 8);
      if (i & 1u) {
        uint32_t value = (uint32_t)old ^ i;
        memcpy(bytes + 12, &value, 4);
      } else {
        uint64_t value = old + x; memcpy(bytes + 7, &value, 8);
      }
    }
    uint64_t a,b; memcpy(&a, bytes + 3, 8); memcpy(&b, bytes + 14, 8);
    for (unsigned j = 0; j != 3; ++j) {
      unsigned char actual[40], wanted[40];
      memset(actual, 0x91, sizeof(actual));
      memset(wanted, 0x91, sizeof(wanted));
      unsigned offset = k & 15u;
      memcpy(wanted + offset, bytes, 24);
      if (functions[j](x,y,n,actual + offset) != (a ^ b) ||
          memcmp(actual,wanted,sizeof(actual))) return 1 + (int)j;
    }
  }
  return 0;
}
)";
  for (auto Level : {"-O0", "-O2"}) {
    auto Executable = tmpFile(std::string("oracle") + Level +
                              neverd::test::executableSuffix());
    auto Built =
        exec(NEVERD_TEST_CLANG,
             {"-std=c11", Level, "-fsanitize=undefined", "-fsanitize-trap=all",
              Original.string(), After.string(), Optimized.string(),
              Harness.string(), "-o", Executable.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    auto Ran = exec(Executable.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.exitCode << " " << Ran.err;
  }
}

TEST_F(ByteCellCompiled, SharedIRCompilesForFourTargetLayouts) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  for (auto Target : {std::pair{"x86_64-unknown-linux-gnu", "e-p:64:64"},
                      {"aarch64-unknown-linux-gnu", "e-p:64:64"},
                      {"aarch64_be-unknown-linux-gnu", "E-p:64:64"},
                      {"armv7-unknown-linux-gnueabihf", "e-p:32:32"}}) {
    SCOPED_TRACE(Target.first);
    llvm::LLVMContext C;
    auto M = parse(C, program(), Target.second);
    ASSERT_TRUE(M);
    auto &F = *M->getFunction("f");
    ASSERT_EQ(ByteCellScalarizationPass::scalarize(F).Objects, 1u);
    promote(F);
    auto Input = tmpFile("cells.ll");
    std::ofstream(Input) << neverd::test::printHostCompilerFixture(*M);
    auto Built = exec(NEVERD_TEST_CLANG,
                      {"-target", Target.first, "-O2", "-c", Input.string(),
                       "-o", tmpFile("cells.o").string()});
    EXPECT_TRUE(Built.ok()) << Built.err;
  }
}
} // namespace neverd::cell_test
