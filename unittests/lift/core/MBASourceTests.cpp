//===- MBASourceTests.cpp - Executable modular MBA source checks ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../NeverDLiftFixture.h"

#include <tuple>

namespace {

using SourceCase = std::tuple<const char *, const char *, bool>;

std::string readSource(const fs::path &Path) {
  std::ifstream Input(Path);
  return std::string(std::istreambuf_iterator<char>(Input), {});
}

std::string functionBody(const std::string &Source, const std::string &Name) {
  const auto NamePos = Source.find(Name + "(");
  EXPECT_NE(NamePos, std::string::npos) << "Missing " << Name << "\n" << Source;
  if (NamePos == std::string::npos)
    return {};
  const auto Begin = Source.find('{', NamePos);
  const auto End = Source.find("\n}", Begin);
  EXPECT_NE(Begin, std::string::npos) << Source;
  EXPECT_NE(End, std::string::npos) << Source;
  if (Begin == std::string::npos || End == std::string::npos)
    return {};
  return Source.substr(Begin, End - Begin);
}

// An unsigned arithmetic oracle keeps the expected answers independent of
// both decompilation paths and of the assembly's Boolean decomposition.
const char *executionHarness() {
  return R"(
#include <inttypes.h>
#include <stdio.h>

static uint64_t value8(uint64_t x, uint64_t y) {
  return (uint32_t)mba_sub8(x, y);
}
static uint64_t value16(uint64_t x, uint64_t y) {
  return (uint32_t)mba_sub16(x, y);
}
static uint64_t value32(uint64_t x, uint64_t y) {
  return (uint32_t)mba_sub32(x, y);
}
static uint64_t value64(uint64_t x, uint64_t y) {
  return (uint64_t)mba_sub64(x, y);
}
static uint64_t disjunction8(uint64_t x, uint64_t y) {
  return (uint32_t)mba_or8(x, y);
}
static uint64_t disjunction16(uint64_t x, uint64_t y) {
  return (uint32_t)mba_or16(x, y);
}
static uint64_t disjunction32(uint64_t x, uint64_t y) {
  return (uint32_t)mba_or32(x, y);
}
static uint64_t disjunction64(uint64_t x, uint64_t y) {
  return (uint64_t)mba_or64(x, y);
}
static uint64_t parity8(uint64_t x, uint64_t y) {
  return mba_parity8(x, y);
}
static uint64_t parity16(uint64_t x, uint64_t y) {
  return mba_parity16(x, y);
}
static uint64_t parity32(uint64_t x, uint64_t y) {
  return mba_parity32(x, y);
}
static uint64_t parity64(uint64_t x, uint64_t y) {
  return mba_parity64(x, y);
}
static uint64_t even_parity(uint64_t value) {
  unsigned ones = 0;
  for (unsigned bit = 0; bit != 8; ++bit)
    ones += (unsigned)((value >> bit) & 1);
  return (ones & 1) == 0;
}
static int check_pair(uint64_t x, uint64_t y) {
  static uint64_t (*const values[])(uint64_t, uint64_t) = {
    value8, value16, value32, value64
  };
  static uint64_t (*const disjunctions[])(uint64_t, uint64_t) = {
    disjunction8, disjunction16, disjunction32, disjunction64
  };
  static uint64_t (*const parities[])(uint64_t, uint64_t) = {
    parity8, parity16, parity32, parity64
  };
  static const uint64_t masks[] = {
    UINT64_C(0xff), UINT64_C(0xffff), UINT64_C(0xffffffff), UINT64_MAX
  };
  for (unsigned i = 0; i != 4; ++i) {
    uint64_t expected = (x - y) & masks[i];
    uint64_t actual = values[i](x, y);
    uint64_t parity = parities[i](x, y);
    if (actual != expected || parity != even_parity(expected)) {
      fprintf(stderr, "width=%u x=%" PRIx64 " y=%" PRIx64
              " value=%" PRIx64 " expected=%" PRIx64 " parity=%" PRIu64 "\n",
              8u << i, x, y, actual, expected, parity);
      return 1;
    }
    uint64_t expected_or = (x | y) & masks[i];
    uint64_t actual_or = disjunctions[i](x, y);
    if (actual_or != expected_or) {
      fprintf(stderr, "OR width=%u x=%" PRIx64 " y=%" PRIx64
              " value=%" PRIx64 " expected=%" PRIx64 "\n",
              8u << i, x, y, actual_or, expected_or);
      return 1;
    }
  }
  uint64_t storage[] = {UINT64_C(0x13579bdf2468ace0), ~(x - y),
                        UINT64_C(0xfedcba9876543210)};
  uint64_t returned = (uint64_t)mba_store64(x, y, &storage[1]);
  if (returned != x - y || storage[1] != x - y ||
      storage[0] != UINT64_C(0x13579bdf2468ace0) ||
      storage[2] != UINT64_C(0xfedcba9876543210)) {
    fprintf(stderr, "store/return mismatch x=%" PRIx64 " y=%" PRIx64 "\n", x, y);
    return 1;
  }
  return 0;
}
int main(void) {
  // Exhaust every pair of bytes, including negative low-byte differences.
  for (uint64_t x = 0; x != 256; ++x) {
    if ((uint64_t)mba_full_product8(x) != x * UINT64_C(255)) {
      fprintf(stderr, "full product incorrectly narrowed at x=%" PRIu64 "\n", x);
      return 1;
    }
    for (uint64_t y = 0; y != 256; ++y)
      if (check_pair(x, y))
        return 1;
  }
  static const uint64_t edges[] = {
    0, 1, 2, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x10000,
    UINT64_C(0x7fffffff), UINT64_C(0x80000000), UINT64_C(0xffffffff),
    UINT64_C(0x100000000), UINT64_C(0x7fffffffffffffff),
    UINT64_C(0x8000000000000000), UINT64_MAX - 1, UINT64_MAX,
    UINT64_C(0xaaaaaaaaaaaaaaaa), UINT64_C(0x5555555555555555)
  };
  for (unsigned i = 0; i != sizeof(edges) / sizeof(edges[0]); ++i)
    for (unsigned j = 0; j != sizeof(edges) / sizeof(edges[0]); ++j)
      if (check_pair(edges[i], edges[j]))
        return 1;
  uint64_t state = UINT64_C(0x92d68ca2f53b17e9);
  for (unsigned i = 0; i != 4096; ++i) {
    state = state * UINT64_C(6364136223846793005) + 1;
    uint64_t x = state;
    state = state * UINT64_C(6364136223846793005) + 1;
    if (check_pair(x, state))
      return 1;
  }
  return 0;
}
)";
}

class MBASourceTest : public NeverDLiftTest,
                      public ::testing::WithParamInterface<SourceCase> {};

TEST_P(MBASourceTest, RemovesModularMBAAndPreservesExecutableBehavior) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "cross-target MBA fixture requires clang";
  const auto &[Format, Triple, LLVM] = GetParam();
  SCOPED_TRACE(std::string(Format) + (LLVM ? " LLVMC" : " HighC"));
  const auto Object = tmpFile("modular-widths.o");
  const auto Compiled =
      exec(NEVERD_TEST_CLANG,
           {"-target", Triple, "-c",
            (fs::path(TEST_SOURCE_DIR) / "x86_64/test_mba_modular_widths.S")
                .string(),
            "-o", Object.string()});
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;

  const auto Output = tmpFile("modular-widths.c");
  std::vector<std::string> Arguments{"decompile"};
  if (LLVM)
    Arguments.push_back("--llvm");
  Arguments.insert(Arguments.end(), {"-o", Output.string(), Object.string()});
  const auto Decompiled = exec(ndBin(), Arguments);
  ASSERT_TRUE(Decompiled.ok()) << Decompiled.err;
  const std::string Source = readSource(Output);
  ASSERT_FALSE(Source.empty());
  for (unsigned Bits : {8u, 16u, 32u, 64u}) {
    const auto Value = functionBody(Source, "mba_sub" + std::to_string(Bits));
    // Casts may remain to express the ABI. Both x - y and -y + x are basic
    // arithmetic; neither needs the original product or Boolean expansion.
    for (char Operator : {'*', '&', '|', '^', '~'})
      EXPECT_EQ(Value.find(Operator), std::string::npos) << Value;
    EXPECT_EQ(std::count(Value.begin(), Value.end(), '-'), 1) << Value;
    EXPECT_LE(std::count(Value.begin(), Value.end(), '+'), 1) << Value;
    EXPECT_EQ(Value.find("128"), std::string::npos) << Value;

    const auto Disjunction =
        functionBody(Source, "mba_or" + std::to_string(Bits));
    for (char Operator : {'*', '+', '-', '&', '^', '~'})
      EXPECT_EQ(Disjunction.find(Operator), std::string::npos) << Disjunction;
    EXPECT_EQ(std::count(Disjunction.begin(), Disjunction.end(), '|'), 1)
        << Disjunction;

    const auto Parity =
        functionBody(Source, "mba_parity" + std::to_string(Bits));
    // Parity legitimately uses masks, XOR, or the complementary byte
    // difference: complementing eight bits preserves even/odd parity.
    for (char Operator : {'*', '|'})
      EXPECT_EQ(Parity.find(Operator), std::string::npos) << Parity;
    EXPECT_EQ(Parity.find("128"), std::string::npos) << Parity;
  }
  const auto Store = functionBody(Source, "mba_store64");
  for (char Operator : {'|', '^', '~'})
    EXPECT_EQ(Store.find(Operator), std::string::npos) << Store;
  EXPECT_EQ(std::count(Store.begin(), Store.end(), '-'), 1) << Store;
  EXPECT_LE(std::count(Store.begin(), Store.end(), '+'), 1) << Store;
  EXPECT_EQ(Store.find("128"), std::string::npos) << Store;
  EXPECT_FALSE(functionBody(Source, "mba_full_product8").empty());

  // This scalar-only fixture needs no SIMD declarations. LLVMC's blanket
  // x86 header otherwise prevents recompiling the source on non-x86 hosts.
  std::ofstream(tmpFile("immintrin.h")).close();
  const auto Harness = tmpFile("execute.c");
  std::ofstream(Harness) << Source << executionHarness();
  for (const char *Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization);
    const auto Executable = tmpFile("execute");
    const auto Recompiled =
        exec(NEVERD_TEST_CLANG,
             {"-std=c11", Optimization, "-fno-inline", "-Werror=return-type",
              "-Werror=implicit-function-declaration", "-fsanitize=undefined",
              "-fsanitize-trap=undefined", "-I", tmp().string(),
              Harness.string(), "-o", Executable.string()});
    ASSERT_TRUE(Recompiled.ok()) << Recompiled.err << "\n" << Source;
    const auto Ran = exec(Executable.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.err << "\n" << Source;
  }
}

INSTANTIATE_TEST_SUITE_P(
    NativeFormats, MBASourceTest,
    ::testing::Values(SourceCase{"ELF", "x86_64-linux-gnu", false},
                      SourceCase{"ELF", "x86_64-linux-gnu", true},
                      SourceCase{"COFF", "x86_64-pc-windows-msvc", false},
                      SourceCase{"COFF", "x86_64-pc-windows-msvc", true},
                      SourceCase{"MachO", "x86_64-apple-macos11", false},
                      SourceCase{"MachO", "x86_64-apple-macos11", true}),
    [](const ::testing::TestParamInfo<SourceCase> &Info) {
      return std::string(std::get<0>(Info.param)) +
             (std::get<2>(Info.param) ? "LLVMC" : "HighC");
    });

} // namespace
