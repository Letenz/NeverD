//===- SmcccCallTests.cpp - SMC Calling Convention contracts --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// An SMC or HVC #0 reads its function identifier and arguments in x0-x7
// (r0-r3 on ARM) and writes its results to x0-x3 (r0-r3).  Both C routes pin
// each value to its register for the instruction, and a function returns a
// result its caller reads.
//
//===----------------------------------------------------------------------===//

#include "CSourceText.h"
#include "NeverDLiftFixture.h"

#include <fstream>
#include <sstream>

namespace {

fs::path a64Obj() { return fs::path(TEST_OBJ_DIR) / "test_smccc_a64.o"; }
fs::path armObj() { return fs::path(TEST_OBJ_DIR) / "test_smccc_arm.o"; }

std::string readText(const fs::path &Path) {
  std::ifstream In(Path);
  std::ostringstream Text;
  Text << In.rdbuf();
  return Text.str();
}

/// The definition of \p Name in C source \p C, from its return type.
std::string definition(const std::string &C, const std::string &Name) {
  const size_t At = neverd::test::cDefinitionAt(C, Name);
  if (At == std::string::npos)
    return {};
  const size_t Begin = C.rfind('\n', At) + 1;
  return C.substr(Begin, C.find("\n}", At) - Begin);
}

} // namespace

class SmcccCall : public NeverDLiftTest {
protected:
  /// The unit both routes write for \p Object.
  std::string source(const fs::path &Object, bool LLVM) {
    const auto R = LLVM ? decompileToC(Object) : decompileToHighC(Object);
    EXPECT_EQ(R.exitCode, 0) << R.err;
    return readText(tmpFile(LLVM ? "decompiled.c" : "decompiled_high.c"));
  }

  void expectCompiles(const std::string &C, const std::string &Name,
                      std::vector<std::string> Target) {
    if (!hasCrossTargetClang())
      return;
    const auto File = tmpFile(Name);
    std::ofstream(File) << C;
    Target.insert(Target.end(),
                  {"-ffreestanding", "-fsyntax-only", File.string()});
    const auto Syntax = exec("clang", Target);
    EXPECT_EQ(Syntax.exitCode, 0) << Syntax.err << "\n" << C;
  }
};

TEST_F(SmcccCall, AArch64CallsPassAndReturnTheirRegisters) {
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    const std::string C = source(a64Obj(), LLVM);
    // The function identifier reaches x0 and the results come back from
    // x1-x3.
    const std::string Trng = definition(C, "trng");
    EXPECT_NE(Trng.find("__asm__(\"x0\") = "), std::string::npos) << C;
    EXPECT_NE(Trng.find(LLVM ? "2214592595" : "0x84000053"), std::string::npos)
        << C;
    EXPECT_NE(Trng.find("\"smc #0x0\""), std::string::npos) << C;
    EXPECT_NE(Trng.find("\"+r\"("), std::string::npos) << C;
    EXPECT_NE(definition(C, "hvc_call").find("\"hvc #0x0\""), std::string::npos)
        << C;
    // A wrapper returns the x0 its caller reads.
    EXPECT_NE(definition(C, "smc_wrap").rfind("void ", 0), 0u) << C;
    EXPECT_EQ(C.find("__builtin_trap"), std::string::npos) << C;
    expectCompiles(C, LLVM ? "smccc_a64_llvm.c" : "smccc_a64_high.c",
                   {"--target=aarch64-linux-gnu"});
  }
}

TEST_F(SmcccCall, ARMCallsPassAndReturnTheirRegisters) {
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    const std::string C = source(armObj(), LLVM);
    const std::string Call = definition(C, "smc_arm");
    EXPECT_NE(Call.find("__asm__(\"r0\") = "), std::string::npos) << C;
    EXPECT_NE(Call.find("\"smc #0x0\""), std::string::npos) << C;
    // r4-r7 are the caller's, which no function can pass.
    EXPECT_EQ(Call.find("__asm__(\"r4\")"), std::string::npos) << C;
    EXPECT_EQ(Call.find("__builtin_trap"), std::string::npos) << C;
    expectCompiles(C, LLVM ? "smccc_arm_llvm.c" : "smccc_arm_high.c",
                   {"--target=armv7a-linux-gnueabihf", "-mcpu=cortex-a15"});
  }
}
