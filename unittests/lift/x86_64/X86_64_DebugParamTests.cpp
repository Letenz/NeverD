//===- X86_64_DebugParamTests.cpp - Debug parameters by location ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// DWARF lists a function's parameters in source order, but System V passes
/// them by class: `f(double x, int n)` has n in EDI and x in XMM0, a 16-byte
/// record arrives in two registers and a complex number in two vector
/// registers.  Each recovered parameter is named after the source parameter
/// the convention put there.  The decompiled functions take their arguments
/// in the same registers, so callers built from the source declarations get
/// the source's results.
///
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include <fstream>
#include <iterator>

class X86_64_DebugParams : public NeverDLiftTest {};

static fs::path debugParamsObj() {
  return fs::path(TEST_OBJ_DIR) / "test_debug_params.o";
}

/// Calls the decompiled functions through their source declarations.
static const char *const SourceCallers = R"c(
struct Pair {
  long a, b;
};
double dp_scale(double x, int n);
long dp_pair_sum(struct Pair p, long k);
double dp_complex_mix(_Complex double z, int n);

int main(void) {
  struct Pair p = {1, 2};
  _Complex double z = 1.5;
  __imag__ z = 2.0;
  if (dp_scale(2.5, 3) != 10.5)
    return 1;
  if (dp_pair_sum(p, 3) != 16)
    return 2;
  if (dp_complex_mix(z, 4) != 8.0)
    return 3;
  return 0;
}
)c";

TEST_F(X86_64_DebugParams, ParametersAreNamedWhereTheConventionPassesThem) {
  ASSERT_TRUE(fs::exists(debugParamsObj())) << debugParamsObj();
  const auto CFile = tmpFile("debug_params.c");
  const auto R = exec(
      ndBin(), {"decompile", "-o", CFile.string(), debugParamsObj().string()});
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::ifstream Ifs(CFile);
  ASSERT_TRUE(Ifs.good()) << CFile;
  const std::string Source((std::istreambuf_iterator<char>(Ifs)),
                           std::istreambuf_iterator<char>());
  for (const char *Definition :
       {"double dp_scale(int32_t n, double x)",
        "int64_t dp_pair_sum(int64_t p, int64_t p_8, int64_t k)",
        "double dp_complex_mix(int32_t n, double z, double z_8)"})
    EXPECT_NE(Source.find(Definition), std::string::npos) << Definition << "\n"
                                                          << Source;

#if defined(_WIN32)
  GTEST_SKIP() << "Microsoft x64 passes arguments by position, not by class";
#endif
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "no C compiler to build the callers";
  const auto Callers = tmpFile("debug_params_callers.c");
  std::ofstream(Callers) << SourceCallers;
  const auto Executable =
      tmpFile(std::string("debug_params") + neverd::test::executableSuffix());
  const auto Built =
      exec(NEVERD_TEST_CLANG,
           {"-std=c11", "-O0", "-fno-strict-aliasing", "-Werror=int-conversion",
            CFile.string(), Callers.string(), "-o", Executable.string()});
  ASSERT_TRUE(Built.ok()) << Built.err << "\n" << Source;
  const auto Run = exec(Executable.string(), {});
  EXPECT_TRUE(Run.ok()) << "exit " << Run.exitCode << "\n" << Source;
}
