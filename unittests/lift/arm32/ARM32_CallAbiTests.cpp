#include "NeverDLiftFixture.h"

#include <fstream>
#include <iterator>
#include <regex>

class ARM32_CallAbi : public NeverDLiftTest {};

// The base AAPCS (softfp) passes an external routine's float arguments in
// r0-r3, not in the VFP registers the image's own code computes in.  Here
// the VFP registers hold x + 1 and x * 0.5 while r0 and r1 carry them with a
// mantissa bit flipped, so binding the call from the VFP registers changes
// what powf receives.
TEST_F(ARM32_CallAbi, SoftFloatExternalTakesItsFloatsInCoreRegisters) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "cross-target compilation requires Clang";
  const std::string Kernel = R"C(
float powf(float, float);
int pw(int a) {
  float x = (float)a;
  union { float f; unsigned u; } b = {x + 1.0f}, e = {x * 0.5f}, r;
  b.u ^= 0x00400000u;
  e.u ^= 0x00200000u;
  r.f = powf(b.f, e.f);
  return (int)r.u;
}
)C";
  const auto Source = tmpFile("softfp-powf.c");
  const auto Object = tmpFile("softfp-powf.o");
  std::ofstream(Source) << Kernel;
  const auto Compiled =
      exec(NEVERD_TEST_CLANG, {"-target", "arm-linux-gnueabi",
                               "-mcpu=cortex-a15", "-mfloat-abi=softfp", "-O2",
                               "-c", Source.string(), "-o", Object.string()});
  ASSERT_EQ(Compiled.exitCode, 0) << Compiled.err;

  const auto Decompiled = decompileToHighC(Object);
  ASSERT_EQ(Decompiled.exitCode, 0) << Decompiled.err;
  std::ifstream Input(tmpFile("decompiled_high.c"));
  ASSERT_TRUE(Input.good());
  const std::string C((std::istreambuf_iterator<char>(Input)),
                      std::istreambuf_iterator<char>());
  ASSERT_EQ(C.find("unknown value"), std::string::npos) << C;

  const std::string Reference = std::regex_replace(
      Kernel, std::regex("\\bpw\\("), std::string("ref_pw("));
  const auto Program = tmpFile("softfp-powf-host.exe");
  std::ofstream(tmpFile("softfp-powf-host.c")) << C << "\n"
                                               << Reference << R"C(
int main(void) {
  static const int Values[] = {-2, 0, 1, 2, 3, 4, 6};
  for (unsigned I = 0; I != sizeof(Values) / sizeof(Values[0]); ++I)
    if (pw(Values[I]) != ref_pw(Values[I]))
      return 1;
  return 0;
}
)C";
  const auto Built =
      exec(NEVERD_TEST_CLANG,
           {"-std=gnu11", "-O2", tmpFile("softfp-powf-host.c").string(), "-lm",
            "-o", Program.string()});
  ASSERT_EQ(Built.exitCode, 0) << Built.err << "\n" << C;
  const auto Run = exec(Program.string(), {});
  EXPECT_EQ(Run.exitCode, 0) << C;
}
