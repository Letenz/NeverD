//===- MachineStringSourceTests.cpp - Repeated memory source execution
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../lift/NeverDLiftFixture.h"

#include "neverd/support/BinaryLoading.h"

namespace {
using namespace neverd;

std::string stringFile(const fs::path &Path) {
  std::ifstream Input(Path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(Input), {});
}

class MachineStringSourceTest : public NeverDLiftTest {
protected:
  static fs::path fixture(const char *Name) {
    return fs::path(TEST_SOURCE_DIR) / "fixtures" / Name;
  }
};

TEST_F(MachineStringSourceTest, NativeTransfersMatchIndependentFullState) {
#if !defined(__x86_64__) || !defined(__linux__)
  GTEST_SKIP() << "native repeated-transfer execution requires x64 Linux";
#endif
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "native repeated-transfer checks require clang";
  const auto Program = tmpFile("native-strings");
  const auto Built =
      exec(NEVERD_TEST_CLANG,
           {"-target", "x86_64-linux-gnu", "-fuse-ld=lld", "-no-pie", "-O2",
            fixture("generic_machine_strings.S").string(),
            fixture("generic_machine_control_observer.S").string(),
            fixture("generic_machine_strings_reference.c").string(), "-o",
            Program.string()});
  ASSERT_TRUE(Built.ok()) << Built.err;
  const auto Ran = exec(Program.string(), {});
  EXPECT_TRUE(Ran.ok()) << Ran.err << " exit=" << Ran.exitCode;
}

class MachineStringRoundTripTest
    : public MachineStringSourceTest,
      public ::testing::WithParamInterface<unsigned> {};

TEST_P(MachineStringRoundTripTest, RecoveredTransfersPreserveFullState) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "repeated-transfer source checks require clang";
  const unsigned Kind = GetParam() / 2;
  const bool LLVM = GetParam() & 1;
  std::string Name = "generic_string_zero";
  if (Kind < 16) {
    Name = (Kind & 2) ? "generic_string_fill_" : "generic_string_move_";
    Name += "bwlq"[Kind / 4];
    Name += (Kind & 1) ? "_b" : "_f";
  }
  const auto Binary = tmpFile("machine-strings.elf");
  const auto Built =
      exec(NEVERD_TEST_CLANG,
           {"-target", "x86_64-linux-gnu", "-fuse-ld=lld", "-nostdlib",
            "-static", "-Wl,-e,generic_string_zero",
            fixture("generic_machine_strings.S").string(),
            fixture("generic_machine_control_observer.S").string(), "-o",
            Binary.string()});
  ASSERT_TRUE(Built.ok()) << Built.err;
  auto Image = loadBinary(Binary);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  va_t ReturnAddress = InvalidVA;
  for (const auto &Symbol : Image->Symbols)
    if (Symbol.Name == "generic_machine_observer_exit")
      ReturnAddress = Symbol.Addr;
  ASSERT_NE(ReturnAddress, InvalidVA);
  const auto Source = tmpFile("recovered.c");
  const auto Report = tmpFile("report.json");
  std::vector<std::string> Args{"decompile",
                                Binary.string(),
                                "--func",
                                Name,
                                "--devirtualize",
                                "--vm-machine-state",
                                "--recovery-report=" + Report.string(),
                                "-o",
                                Source.string()};
  if (LLVM)
    Args.push_back("--llvm");
  const auto Recovered = exec(ndBin(), Args);
  ASSERT_TRUE(Recovered.ok()) << Recovered.err << stringFile(Report);
  const auto Harness = tmpFile("reference.c");
  std::ofstream(Harness) << stringFile(Source)
                         << "\n#define GENERIC_STRING_SOURCE\n"
                         << "#define GENERIC_STRING_FUNCTION " << Name << "\n"
                         << "#define GENERIC_STRING_KIND " << Kind << "\n"
                         << "#define STRING_RETURN_ADDRESS UINT64_C("
                         << ReturnAddress << ")\n"
                         << stringFile(
                                fixture("generic_machine_strings_reference.c"));
  std::ofstream(tmpFile("immintrin.h")).close();
  for (const char *Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization);
    const auto Program = tmpFile(std::string("recovered-strings") +
                                 neverd::test::executableSuffix());
    const auto Compiled =
        exec(NEVERD_TEST_CLANG,
             {"-std=c11", Optimization, "-Werror=return-type",
              "-Werror=implicit-function-declaration", "-fsanitize=undefined",
              "-fsanitize-trap=undefined", "-I", tmp().string(),
              Harness.string(), "-o", Program.string()});
    ASSERT_TRUE(Compiled.ok()) << Compiled.err << stringFile(Source);
    const auto Ran = exec(Program.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.err << " exit=" << Ran.exitCode;
  }
}

INSTANTIATE_TEST_SUITE_P(PublicRepeatedTransfers, MachineStringRoundTripTest,
                         ::testing::Range(0u, 34u));

} // namespace
