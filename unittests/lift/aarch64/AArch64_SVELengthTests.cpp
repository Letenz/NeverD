#include "NeverDLiftFixture.h"

class AArch64_SVELength : public NeverDLiftTest {
protected:
  void expectPairedClangSyntax(const fs::path &CFile,
                               const std::string &Source) {
    auto syntax = checkHighCClangSyntax(
        CFile, {"-target", "aarch64-none-elf", "-ffreestanding",
                "-march=armv8.2-a+sve", "-std=gnu11"});
    EXPECT_EQ(syntax.exitCode, 0) << syntax.err << "\n" << Source;
  }

  void expectVectorLengthResults(const std::string &Functions,
                                 const std::string &Checks) {
    if (!hasCrossTargetClang())
      GTEST_SKIP() << "SVE HighC execution requires Clang";
    // Execute unchanged function bodies on the host with the architectural
    // count intrinsics supplied by a fixture. Cover every legal SVE vector
    // length; exact spelling of the generated integer casts is immaterial.
    const auto Source = tmpFile("sve_length_runtime.c");
    std::ofstream Out(Source);
    Out << "#include <stdint.h>\n"
           "static uint64_t vector_bytes;\n"
           "static uint64_t svcntb(void) { return vector_bytes; }\n"
           "static uint64_t svcntw(void) { return vector_bytes / 4; }\n"
        << Functions
        << "\nint main(void) {\n"
           "  for (vector_bytes = 16; vector_bytes <= 256; vector_bytes += 16) "
           "{\n"
        << Checks << "\n  }\n  return 0;\n}\n";
    Out.close();
    ASSERT_TRUE(Out.good());
    const auto Executable = tmpFile("sve_length_runtime.exe");
    const auto Compile =
        exec(NEVERD_TEST_CLANG,
             {"-std=gnu11", "-O2", "-fsanitize=signed-integer-overflow",
              "-fsanitize-trap=signed-integer-overflow", Source.string(), "-o",
              Executable.string()});
    ASSERT_EQ(Compile.exitCode, 0) << Compile.err;
    const auto Run = exec(Executable.string(), {});
    EXPECT_EQ(Run.exitCode, 0) << Run.err << "\n" << Functions;
  }
};

static fs::path sveLengthObj() {
  return fs::path(TEST_OBJ_DIR) / "test_sve_length_a64.o";
}

static std::string sveLengthFunctionIR(const std::string &IR,
                                       const std::string &Name) {
  auto NamePos = IR.find("@" + Name + "(");
  if (NamePos == std::string::npos)
    return {};
  auto Begin = IR.rfind("define ", NamePos);
  auto End = IR.find("\n}", NamePos);
  if (Begin == std::string::npos || End == std::string::npos)
    return {};
  return IR.substr(Begin, End + 2 - Begin);
}

static std::string sveLengthFunctionC(const std::string &Source,
                                      const std::string &Name) {
  auto NamePos = Source.find(Name + "(");
  if (NamePos == std::string::npos)
    return {};
  auto Begin = Source.rfind('\n', NamePos);
  auto End = Source.find("\n}", NamePos);
  if (End == std::string::npos)
    return {};
  Begin = Begin == std::string::npos ? 0 : Begin + 1;
  return Source.substr(Begin, End + 2 - Begin);
}

TEST_F(AArch64_SVELength, CntbAndIncbUseRuntimeVectorLength) {
  auto r = liftToLLVMIR(sveLengthObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;
  auto F = sveLengthFunctionIR(r.out, "test_sve_length_a64");
  ASSERT_FALSE(F.empty()) << r.out;

  EXPECT_NE(F.find("@llvm.aarch64.sve.cntb(i32 31)"), std::string::npos) << F;
  EXPECT_EQ(F.find("store i64 16"), std::string::npos) << F;
  EXPECT_EQ(F.find("add i64 %arg0, 1"), std::string::npos) << F;
}

TEST_F(AArch64_SVELength, IncAndDecHonorEncodedMultiplier) {
  auto r = liftToLLVMIR(sveLengthObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto Inc = sveLengthFunctionIR(r.out, "test_sve_incb_mul2");
  ASSERT_FALSE(Inc.empty()) << r.out;
  EXPECT_NE(Inc.find("@llvm.aarch64.sve.cntb(i32 31)"), std::string::npos)
      << Inc;
  EXPECT_NE(Inc.find("shl i64 %svcnt, 1"), std::string::npos) << Inc;

  auto Dec = sveLengthFunctionIR(r.out, "test_sve_decw_mul4");
  ASSERT_FALSE(Dec.empty()) << r.out;
  EXPECT_NE(Dec.find("@llvm.aarch64.sve.cntw(i32 31)"), std::string::npos)
      << Dec;
  EXPECT_NE(Dec.find("shl i64 %svcnt, 2"), std::string::npos) << Dec;
}

TEST_F(AArch64_SVELength, AddvlScalesSignedImmediateByRuntimeVectorLength) {
  auto r = liftToLLVMIR(sveLengthObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto Positive = sveLengthFunctionIR(r.out, "test_sve_addvl_two");
  ASSERT_FALSE(Positive.empty()) << r.out;
  EXPECT_NE(Positive.find("@llvm.aarch64.sve.cntb(i32 31)"), std::string::npos)
      << Positive;
  EXPECT_NE(Positive.find("shl i64 %svcnt, 1"), std::string::npos) << Positive;

  auto Negative = sveLengthFunctionIR(r.out, "test_sve_addvl_negative");
  ASSERT_FALSE(Negative.empty()) << r.out;
  EXPECT_NE(Negative.find("@llvm.aarch64.sve.cntb(i32 31)"), std::string::npos)
      << Negative;
  EXPECT_NE(Negative.find("-3"), std::string::npos) << Negative;
}

TEST_F(AArch64_SVELength, HighCUsesSVEACLEAndCompiles) {
  auto r = decompileToHighC(sveLengthObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
  EXPECT_NE(source.find("#include <arm_sve.h>"), std::string::npos) << source;
  EXPECT_NE(source.find("svcntb()"), std::string::npos) << source;
  EXPECT_NE(source.find("arg0"), std::string::npos) << source;
  EXPECT_EQ(source.find("return 17"), std::string::npos) << source;

  expectPairedClangSyntax(cFile, source);
}

TEST_F(AArch64_SVELength, HighCHonorsEncodedMultiplierAndCompiles) {
  auto r = decompileToHighC(sveLengthObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());

  auto Inc = sveLengthFunctionC(source, "test_sve_incb_mul2");
  ASSERT_FALSE(Inc.empty()) << source;
  EXPECT_NE(Inc.find("svcntb()"), std::string::npos) << Inc;

  auto Dec = sveLengthFunctionC(source, "test_sve_decw_mul4");
  ASSERT_FALSE(Dec.empty()) << source;
  EXPECT_NE(Dec.find("svcntw()"), std::string::npos) << Dec;

  expectVectorLengthResults(
      Inc + "\n" + Dec,
      "if ((uint64_t)test_sve_incb_mul2() != 10 + 2 * vector_bytes) return 1;\n"
      "if ((uint64_t)test_sve_decw_mul4() != 100 - 4 * (vector_bytes / 4)) "
      "return 2;");

  expectPairedClangSyntax(cFile, source);
}

TEST_F(AArch64_SVELength, HighCAddvlUsesRuntimeVectorLengthAndCompiles) {
  auto r = decompileToHighC(sveLengthObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());

  auto Positive = sveLengthFunctionC(source, "test_sve_addvl_two");
  ASSERT_FALSE(Positive.empty()) << source;
  EXPECT_NE(Positive.find("svcntb()"), std::string::npos) << Positive;

  auto Negative = sveLengthFunctionC(source, "test_sve_addvl_negative");
  ASSERT_FALSE(Negative.empty()) << source;
  EXPECT_NE(Negative.find("svcntb()"), std::string::npos) << Negative;

  expectVectorLengthResults(
      Positive + "\n" + Negative,
      "if ((uint64_t)test_sve_addvl_two() != 10 + 2 * vector_bytes) return 1;\n"
      "if ((uint64_t)test_sve_addvl_negative() != 100 - 3 * vector_bytes) "
      "return 2;");

  expectPairedClangSyntax(cFile, source);
}
