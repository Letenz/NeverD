#include "NeverDLiftFixture.h"

#include <cstdint>
#include <fstream>

// Native x86_64 host-CPU oracle for lift / decompile / identity-patch.
// ENTERW must preserve RBP[63:16]; a pointer-width ENTER clobbers them to a
// stack address and the process exit status changes.

class X86_64_NativeHostMode : public NeverDLiftTest {
protected:
  static bool hostIsX86_64() {
#if defined(__x86_64__) || defined(_M_X64)
    return true;
#else
    return false;
#endif
  }
};

static const char kEnterWSrc[] = R"(
unsigned long f(void) {
  unsigned long d;
  __asm__ volatile(
      "pushq %%rbp\n\t"
      "movq %%rsp, %%rcx\n\t"
      "movq $0xAAAABBBBCCCCDDDD, %%rbp\n\t"
      "data16 enter $0, $1\n\t"
      "movq %%rbp, %0\n\t"
      "movq %%rcx, %%rsp\n\t"
      "popq %%rbp\n\t"
      : "=r"(d)
      :
      : "rcx", "cc", "memory");
  return d >> 16;
}

void _start(void) {
  unsigned long r = f();
  unsigned long code = r & 0xff;
  __asm__ volatile("movq %0, %%rdi\n\t"
                   "movq $60, %%rax\n\tsyscall"
                   :
                   : "r"(code)
                   : "rax", "rdi", "rcx", "r11", "memory");
}
)";

TEST_F(X86_64_NativeHostMode, EnterWLiftDecompilePatchAgreeWithCpu) {
  if (!hostIsX86_64())
    GTEST_SKIP() << "native x86_64 host execution required";
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "clang required to build the native x86_64 fixture";

  auto Src = tmpFile("enterw_host.c");
  {
    std::ofstream OS(Src);
    OS << kEnterWSrc;
  }
  auto Orig = tmpFile("enterw_host");
  auto CR = exec(NEVERD_TEST_CLANG,
                 {"-O0", "-nostdlib", "-static", "-fno-stack-protector",
                  "-fno-asynchronous-unwind-tables", "-fno-unwind-tables",
                  "-o", Orig.string(), Src.string()});
  ASSERT_EQ(CR.exitCode, 0) << "native fixture compile failed: " << CR.err;
  ASSERT_TRUE(fs::exists(Orig));
  EXPECT_GT(fs::file_size(Orig), 0u);

  auto runNative = [&](const fs::path &Bin) {
    return neverd::test::systemExitCode(
        neverd::test::runShellCommand(neverd::test::shellQuote(Bin.string())));
  };

  const int OrigStatus = runNative(Orig);
  ASSERT_EQ(OrigStatus, 0xcc) << "host CPU ENTERW should keep RBP high 0xCC";

  auto Lift = exec(ndBin(), {"lift", Orig.string()});
  ASSERT_EQ(Lift.exitCode, 0) << "neverd lift failed: " << Lift.err;
  EXPECT_FALSE(Lift.out.empty());
  EXPECT_NE(Lift.out.find("define"), std::string::npos) << Lift.out;

  auto HighC = tmpFile("enterw_host.c.decompiled");
  auto DC = exec(ndBin(),
                 {"decompile", "-o", HighC.string(), Orig.string()});
  ASSERT_EQ(DC.exitCode, 0) << "neverd decompile failed: " << DC.err;
  ASSERT_TRUE(fs::exists(HighC));
  EXPECT_GT(fs::file_size(HighC), 0u);

  auto LLVMC = tmpFile("enterw_host.llvm.c");
  auto DL = exec(ndBin(), {"decompile", "--llvm", "-o", LLVMC.string(),
                           Orig.string()});
  ASSERT_EQ(DL.exitCode, 0) << "neverd decompile --llvm failed: " << DL.err;
  ASSERT_TRUE(fs::exists(LLVMC));
  EXPECT_GT(fs::file_size(LLVMC), 0u);

  auto PatchR = patchBinary(Orig);
  ASSERT_EQ(PatchR.exitCode, 0) << "neverd patch failed: " << PatchR.err;
  auto Patched = tmpFile("patched");
  ASSERT_TRUE(fs::exists(Patched));
  EXPECT_GT(fs::file_size(Patched), 0u);

  auto Relift = liftToLowIR(Patched);
  ASSERT_EQ(Relift.exitCode, 0) << "patched ELF should still lift: " << Relift.err;

  const int PatchedStatus = runNative(Patched);
  EXPECT_EQ(PatchedStatus, OrigStatus)
      << "identity patch must preserve native ENTERW result";
}

// clang -O2 `movapd; cvtsi2sd; mulpd` consumes xmm[127:64] after the
// convert.  Hardware keeps the high lane; a lift that zeros it returns 0
// instead of 4 after (2.0*2.0).
static const char kCvtSI2SDMulpdSrc[] = R"(
unsigned long f(void) {
  unsigned long out;
  unsigned long a = 3;
  unsigned long b = 0x4000000000000000ULL;
  __asm__ volatile(
      "movq %1, %%xmm5\n\t"
      "unpcklpd %%xmm5, %%xmm5\n\t"
      "cvtsi2sd %2, %%xmm5\n\t"
      "mulpd %%xmm5, %%xmm5\n\t"
      "movhlps %%xmm5, %%xmm5\n\t"
      "cvttsd2si %%xmm5, %0\n\t"
      : "=r"(out)
      : "r"(b), "r"(a)
      : "xmm5");
  return out;
}

void _start(void) {
  unsigned long r = f();
  __asm__ volatile("movq %0, %%rdi\n\t"
                   "movq $60, %%rax\n\tsyscall"
                   :
                   : "r"(r)
                   : "rax", "rdi", "rcx", "r11", "memory");
}
)";

TEST_F(X86_64_NativeHostMode, CvtSI2SDMulpdPatchAgreesWithCpu) {
  if (!hostIsX86_64())
    GTEST_SKIP() << "native x86_64 host execution required";
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "clang required to build the native x86_64 fixture";

  auto Src = tmpFile("cvtsi2sd_mulpd_host.c");
  {
    std::ofstream OS(Src);
    OS << kCvtSI2SDMulpdSrc;
  }
  auto Orig = tmpFile("cvtsi2sd_mulpd_host");
  auto CR = exec(NEVERD_TEST_CLANG,
                 {"-O0", "-nostdlib", "-static", "-fno-stack-protector",
                  "-fno-asynchronous-unwind-tables", "-fno-unwind-tables",
                  "-o", Orig.string(), Src.string()});
  ASSERT_EQ(CR.exitCode, 0) << "native fixture compile failed: " << CR.err;
  ASSERT_TRUE(fs::exists(Orig));

  auto runNative = [&](const fs::path &Bin) {
    return neverd::test::systemExitCode(
        neverd::test::runShellCommand(neverd::test::shellQuote(Bin.string())));
  };

  const int OrigStatus = runNative(Orig);
  ASSERT_EQ(OrigStatus, 4)
      << "host CPU must preserve xmm[127:64] through CVTSI2SD then MULPD";

  auto PatchR = patchBinary(Orig);
  ASSERT_EQ(PatchR.exitCode, 0) << "neverd patch failed: " << PatchR.err;
  auto Patched = tmpFile("patched");
  ASSERT_TRUE(fs::exists(Patched));
  EXPECT_GT(fs::file_size(Patched), 0u);

  const int PatchedStatus = runNative(Patched);
  EXPECT_EQ(PatchedStatus, OrigStatus)
      << "identity patch must preserve native CVTSI2SD+MULPD high-lane result";
}
