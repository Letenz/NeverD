#include "NeverDLiftFixture.h"

#include <cstdint>
#include <fstream>

// Native i386 host-CPU oracle for lift / decompile / identity-patch.
// The host can run 32-bit userspace; Unicorn-only roundtrips are not enough.

class X86_32_NativeHostMode : public NeverDLiftTest {
protected:
  static bool hostCanRunI386() {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) ||             \
    defined(_M_IX86)
    return true;
#else
    return false;
#endif
  }

  RunResult compileI386(const fs::path &Src, const fs::path &Out) const {
    if (hasCrossTargetClang()) {
      auto CR = exec(NEVERD_TEST_CLANG,
                     {"-m32", "-O0", "-nostdlib", "-static",
                      "-fno-stack-protector", "-fno-asynchronous-unwind-tables",
                      "-fno-unwind-tables", "-o", Out.string(), Src.string()});
      if (CR.exitCode == 0)
        return CR;
    }
    return exec("gcc",
                {"-m32", "-O0", "-nostdlib", "-static", "-fno-stack-protector",
                 "-o", Out.string(), Src.string()});
  }
};

static const char kAdd42Src[] = R"(
int add(int a, int b) { return a + b; }
void _start(void) {
  int r = add(20, 22);
  __asm__ volatile("movl %0, %%ebx\n\t"
                   "movl $1, %%eax\n\t"
                   "int $0x80"
                   :
                   : "r"(r)
                   : "eax", "ebx", "memory");
}
)";

TEST_F(X86_32_NativeHostMode, IdentityPatchAgreesWithCpu) {
  if (!hostCanRunI386())
    GTEST_SKIP() << "native i386 host execution required";

  auto Src = tmpFile("i386_add_host.c");
  {
    std::ofstream OS(Src);
    OS << kAdd42Src;
  }
  auto Orig = tmpFile("i386_add_host");
  auto CR = compileI386(Src, Orig);
  if (CR.exitCode != 0)
    GTEST_SKIP() << "i386 compile unavailable: " << CR.err;
  ASSERT_TRUE(fs::exists(Orig));
  EXPECT_GT(fs::file_size(Orig), 0u);

  auto runNative = [&](const fs::path &Bin) {
    return neverd::test::systemExitCode(
        neverd::test::runShellCommand(neverd::test::shellQuote(Bin.string())));
  };

  const int OrigStatus = runNative(Orig);
  ASSERT_EQ(OrigStatus, 42) << "host CPU i386 add(20,22) must return 42";

  auto Lift = exec(ndBin(), {"lift", Orig.string()});
  ASSERT_EQ(Lift.exitCode, 0) << "neverd lift failed: " << Lift.err;
  EXPECT_FALSE(Lift.out.empty());
  EXPECT_NE(Lift.out.find("define"), std::string::npos) << Lift.out;

  auto HighC = tmpFile("i386_add_host.c.decompiled");
  auto DC = exec(ndBin(), {"decompile", "-o", HighC.string(), Orig.string()});
  ASSERT_EQ(DC.exitCode, 0) << "neverd decompile failed: " << DC.err;
  ASSERT_TRUE(fs::exists(HighC));
  EXPECT_GT(fs::file_size(HighC), 0u);

  auto LLVMC = tmpFile("i386_add_host.llvm.c");
  auto DL = exec(ndBin(),
                 {"decompile", "--llvm", "-o", LLVMC.string(), Orig.string()});
  ASSERT_EQ(DL.exitCode, 0) << "neverd decompile --llvm failed: " << DL.err;
  ASSERT_TRUE(fs::exists(LLVMC));
  EXPECT_GT(fs::file_size(LLVMC), 0u);

  auto PatchR = patchBinary(Orig);
  ASSERT_EQ(PatchR.exitCode, 0) << "neverd patch failed: " << PatchR.err;
  auto Patched = tmpFile("patched");
  ASSERT_TRUE(fs::exists(Patched));
  EXPECT_GT(fs::file_size(Patched), 0u);

  auto Relift = liftToLowIR(Patched);
  ASSERT_EQ(Relift.exitCode, 0)
      << "patched i386 ELF should still lift: " << Relift.err;

  const int PatchedStatus = runNative(Patched);
  EXPECT_EQ(PatchedStatus, OrigStatus)
      << "identity patch must preserve native i386 add result";
}

// 16-bit ENTER on i386 writes BP only; a pointer-width ENTER clobbers
// EBP[31:16] to a stack address and the process exit status changes.
static const char kEnterWSrc[] = R"(
unsigned f(void) {
  unsigned d;
  __asm__ volatile(
      "pushl %%ebp\n\t"
      "movl %%esp, %%ecx\n\t"
      "movl $0xAABBCCDD, %%ebp\n\t"
      "data16 enter $0, $1\n\t"
      "movl %%ebp, %0\n\t"
      "movl %%ecx, %%esp\n\t"
      "popl %%ebp\n\t"
      : "=r"(d)
      :
      : "ecx", "cc", "memory");
  return d >> 16;
}

void _start(void) {
  unsigned r = f();
  unsigned code = r & 0xff;
  __asm__ volatile("movl %0, %%ebx\n\t"
                   "movl $1, %%eax\n\t"
                   "int $0x80"
                   :
                   : "r"(code)
                   : "eax", "ebx", "memory");
}
)";

TEST_F(X86_32_NativeHostMode, EnterWPatchAgreesWithCpu) {
  if (!hostCanRunI386())
    GTEST_SKIP() << "native i386 host execution required";

  auto Src = tmpFile("i386_enterw_host.c");
  {
    std::ofstream OS(Src);
    OS << kEnterWSrc;
  }
  auto Orig = tmpFile("i386_enterw_host");
  auto CR = compileI386(Src, Orig);
  if (CR.exitCode != 0)
    GTEST_SKIP() << "i386 compile unavailable: " << CR.err;
  ASSERT_TRUE(fs::exists(Orig));

  auto runNative = [&](const fs::path &Bin) {
    return neverd::test::systemExitCode(
        neverd::test::runShellCommand(neverd::test::shellQuote(Bin.string())));
  };

  const int OrigStatus = runNative(Orig);
  ASSERT_EQ(OrigStatus, 0xbb) << "host CPU ENTERW should keep EBP high 0xBB";

  auto PatchR = patchBinary(Orig);
  ASSERT_EQ(PatchR.exitCode, 0) << "neverd patch failed: " << PatchR.err;
  auto Patched = tmpFile("patched");
  ASSERT_TRUE(fs::exists(Patched));

  const int PatchedStatus = runNative(Patched);
  EXPECT_EQ(PatchedStatus, OrigStatus)
      << "identity patch must preserve native i386 ENTERW result";
}
