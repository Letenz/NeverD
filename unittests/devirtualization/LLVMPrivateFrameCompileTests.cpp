//===- LLVMPrivateFrameCompileTests.cpp - Independent memory oracles ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../lift/NeverDLiftFixture.h"
#include "LLVMPrivateFrameProjectionTest.h"

namespace neverd::analysis::frame_test {
class LLVMPrivateFrameCompiled : public NeverDLiftTest {};

TEST_F(LLVMPrivateFrameCompiled, OverlappingBytesAndAliasedOutputsAtO0AndO2) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  if (sizeof(uintptr_t) != 8)
    GTEST_SKIP() << "The runtime oracle uses a 64-bit host";
  llvm::LLVMContext C;
  auto M = parse(C, program());
  ASSERT_TRUE(M);
  auto &F = *M->getFunction("f");
  F.setName("before");
  auto Original = tmpFile("before.ll");
  std::ofstream(Original) << print(*M);
  auto R = projectLLVMPrivateFrame(F, contract(F));
  ASSERT_EQ(R.Status, LLVMPrivateFrameResult::Projected) << R.Diagnostic;
  F.setName("after");
  auto Projected = tmpFile("after.ll");
  std::ofstream(Projected) << print(*M);
  auto Harness = tmpFile("oracle.c");
  std::ofstream(Harness) << R"(
#include <stdint.h>
#include <string.h>
extern uint64_t before(uint64_t, uint32_t, uint32_t, void *, void *);
extern uint64_t after(uint64_t, uint32_t, uint32_t, void *, void *);
static uint32_t state = 0x321ab541u;
static uint32_t word(void) {
  state = state * 1664525u + 1013904223u;
  return state;
}
int main(void) {
  unsigned char frame[256], initial[256], expected[256];
  unsigned char output[48], input[48], wanted[48];
  for (unsigned k = 0; k != 2048; ++k) {
    uint32_t x = k < 256 ? k : word();
    uint32_t n = k < 256 ? 255u - k : word();
    unsigned at = 128 + (k & 15u);
    uint64_t root = (uint64_t)(uintptr_t)(frame + at);
    for (unsigned j = 0; j != sizeof(frame); ++j) initial[j] = (unsigned char)word();
    for (unsigned j = 0; j != sizeof(input); ++j) input[j] = (unsigned char)word();
    uint64_t packed = (uint64_t)x ^ UINT64_C(619083122760);
    uint16_t half = (uint16_t)n;
    memcpy((unsigned char *)&packed + 2, &half, 2);
    uint32_t value = (n & 1u) ? (x ^ 39u) : (x + 23u);
    for (uint32_t i = 0; i < (n & 7u); ++i)
      value = (i & 1u) ? (value ^ i) : (value + x);
    memcpy(expected, initial, sizeof(frame));
    memcpy(expected + at - 48, &packed, 8);
    memcpy(expected + at - 20, &value, 4);
    const unsigned aliases[] = {0, 8, 12, 24};
    for (unsigned a = 0; a != 4; ++a) {
      memcpy(wanted, input, sizeof(input));
      memcpy(wanted, &root, 8);
      uint32_t other;
      memcpy(&other, wanted + aliases[a], 4);
      uint32_t combined = value ^ other;
      memcpy(wanted + 12, &combined, 4);
      memcpy(wanted + 24, &packed, 8);
      uint64_t answer = root ^ packed ^ (uint64_t)combined;
      memcpy(frame, initial, sizeof(frame));
      memcpy(output, input, sizeof(input));
      if (before(root, x, n, output, output + aliases[a]) != answer ||
          memcmp(output, wanted, sizeof(output)) ||
          memcmp(frame, expected, sizeof(frame))) return 1;
      memcpy(frame, initial, sizeof(frame));
      memcpy(output, input, sizeof(input));
      if (after(root, x, n, output, output + aliases[a]) != answer ||
          memcmp(output, wanted, sizeof(output)) ||
          memcmp(frame, initial, sizeof(frame))) return 2;
    }
  }
  return 0;
}
)";
  for (const char *Level : {"-O0", "-O2"}) {
    SCOPED_TRACE(Level);
    auto Executable = tmpFile(std::string("frame") + Level +
                              neverd::test::executableSuffix());
    auto Built =
        exec(NEVERD_TEST_CLANG,
             {"-std=c11", Level, "-fsanitize=undefined", "-fsanitize-trap=all",
              Original.string(), Projected.string(), Harness.string(), "-o",
              Executable.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    auto Ran = exec(Executable.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.exitCode << " " << Ran.err;
  }
}

TEST_F(LLVMPrivateFrameCompiled, CompilesSharedProjectionForFourTargetLayouts) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  struct Target {
    const char *Triple;
    unsigned Width;
    bool BigEndian;
  };
  for (auto T : {Target{"x86_64-unknown-linux-gnu", 64, false},
                 Target{"aarch64-unknown-linux-gnu", 64, false},
                 Target{"aarch64_be-unknown-linux-gnu", 64, true},
                 Target{"armv7-unknown-linux-gnueabihf", 32, false}}) {
    SCOPED_TRACE(T.Triple);
    llvm::LLVMContext C;
    auto M = parse(C, program(T.Width), T.Width, T.BigEndian);
    ASSERT_TRUE(M);
    auto &F = *M->getFunction("f");
    auto R = projectLLVMPrivateFrame(F, contract(F));
    ASSERT_EQ(R.Status, LLVMPrivateFrameResult::Projected) << R.Diagnostic;
    ASSERT_FALSE(llvm::verifyModule(*M, &llvm::errs()));
    auto Input = tmpFile("input.ll");
    std::ofstream(Input) << print(*M);
    auto Built = exec(NEVERD_TEST_CLANG,
                      {"-target", T.Triple, "-O2", "-c", Input.string(), "-o",
                       tmpFile("output.o").string()});
    EXPECT_TRUE(Built.ok()) << Built.err;
  }
}
} // namespace neverd::analysis::frame_test
