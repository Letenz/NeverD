//===- HalfFMAHostTests.cpp - Independent host binary16 FMA oracle --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The test-only host provider must match APFloat after exactly one rounding.
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"

#include <array>

TEST_F(NeverDLiftTest, HostHalfFMAProviderMatchesIndependentIEEEOracle) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "binary16 host-provider execution requires Clang";
  const auto CFile = tmpFile("half-fma-host.c");
  std::ofstream Out(CFile);
  Out << "#include <stdint.h>\n"
         "extern _Float16 fmaf16(_Float16, _Float16, _Float16);\n"
         "static const uint16_t cases[][4] = {\n";
  const auto Add = [&](uint16_t A, uint16_t B, uint16_t C) {
    llvm::APFloat Value(llvm::APFloat::IEEEhalf(), llvm::APInt(16, A));
    const llvm::APFloat Other(llvm::APFloat::IEEEhalf(), llvm::APInt(16, B));
    const llvm::APFloat Addend(llvm::APFloat::IEEEhalf(), llvm::APInt(16, C));
    Value.fusedMultiplyAdd(Other, Addend, llvm::APFloat::rmNearestTiesToEven);
    Out << '{' << A << ',' << B << ',' << C << ','
        << Value.bitcastToAPInt().getZExtValue() << "},\n";
  };
  const std::array<uint16_t, 14> Edges = {
      0,      0x8000, 1,      0x8001, 0x03ff, 0x0400, 0x3bff,
      0x3c00, 0x3c01, 0xbc00, 0x7bff, 0xfbff, 0x7c00, 0xfc00};
  for (uint16_t A : Edges)
    for (uint16_t B : Edges)
      for (uint16_t C : Edges)
        Add(A, B, C);
  uint64_t Random = 0x347bd91a584e6c01;
  for (unsigned I = 0; I != 8192; ++I) {
    Random = Random * UINT64_C(6364136223846793005) + 1;
    Add(uint16_t(Random), uint16_t(Random >> 16), uint16_t(Random >> 32));
  }
  Out << R"C(};
int main(void) {
  _Float16 (*volatile operation)(_Float16, _Float16, _Float16) = fmaf16;
  for (unsigned i = 0; i != sizeof(cases) / sizeof(cases[0]); ++i) {
    uint16_t actual = __builtin_bit_cast(uint16_t, operation(
        __builtin_bit_cast(_Float16, cases[i][0]),
        __builtin_bit_cast(_Float16, cases[i][1]),
        __builtin_bit_cast(_Float16, cases[i][2])));
    uint16_t expected = cases[i][3];
    // IEEE permits NaN payload choices; require a quiet NaN for those cases.
    if ((expected & 0x7fff) > 0x7c00) {
      if ((actual & 0x7e00) != 0x7e00) return 1;
    } else if (actual != expected) return 2;
  }
  return 0;
}
)C";
  Out.close();
  const auto Provider =
      fs::path(TEST_SOURCE_DIR) / "core/fixtures/half_fma_host.c";
  ASSERT_TRUE(fs::exists(Provider));
  const auto Program = tmpFile("half-fma-host.exe");
  for (const char *Level : {"-O0", "-O2"}) {
    SCOPED_TRACE(Level);
    const auto Built = exec(NEVERD_TEST_CLANG,
                            {"-std=gnu11", Level, CFile.string(),
                             Provider.string(), "-lm", "-o", Program.string()});
    ASSERT_EQ(Built.exitCode, 0) << Built.err;
    const auto Executed = exec(Program.string(), {});
    EXPECT_EQ(Executed.exitCode, 0) << Executed.err;
  }
}
