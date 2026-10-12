//===- X86_64_CallAbiTests.cpp - x86-64 call arguments in HighC -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// System V x86-64 passes the seventh and later integer arguments on the
// stack, and a callee can come from a stack slot.  Each kernel is decompiled
// to HighC and run on the host against its own C.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

#include <fstream>
#include <iterator>
#include <regex>
#include <string>

class X86_64_CallAbi : public HighCHostExecutionTest {
protected:
  /// Lifts \p Source's object to LLVM IR, links it with \p Source renamed,
  /// and compares \p Entry with the original over a range of arguments.
  /// \p Callee is the source's other function.
  void expectLiftedRunsLikeSource(const std::string &Source,
                                  const std::string &Entry,
                                  const std::string &Callee) {
    if (!hasCrossTargetClang())
      GTEST_SKIP() << "cross-target compilation requires Clang";
    const auto SourceFile = tmpFile(Entry + ".c");
    const auto Object = tmpFile(Entry + ".o");
    const auto IR = tmpFile(Entry + ".ll");
    std::ofstream(SourceFile) << Source;
    const auto Compiled =
        exec(NEVERD_TEST_CLANG, {"-target", "x86_64-linux-gnu", "-O2", "-c",
                                 SourceFile.string(), "-o", Object.string()});
    ASSERT_EQ(Compiled.exitCode, 0) << Compiled.err;
    const auto Lifted =
        exec(ndBin(), {"lift", Object.string(), "-o", IR.string()});
    ASSERT_EQ(Lifted.exitCode, 0) << Lifted.err;
    const auto Reference = tmpFile(Entry + "_ref.c");
    const auto Driver = tmpFile(Entry + "_driver.c");
    const auto Program = tmpFile(Entry + ".exe");
    std::ofstream(Reference) << std::regex_replace(
        std::regex_replace(Source, std::regex("\\b" + Entry + "\\b"),
                           "ref_" + Entry),
        std::regex("\\b" + Callee + "\\b"), "ref_" + Callee);
    std::ofstream(Driver) << "int " << Entry << "(int);\nint ref_" << Entry
                          << "(int);\n"
                          << "int main(void) {\n"
                          << "  for (int a = -3; a < 50; a += 7)\n"
                          << "    if (" << Entry << "(a) != ref_" << Entry
                          << "(a))\n"
                          << "      return 1;\n"
                          << "  return 0;\n"
                          << "}\n";
    const auto Built =
        exec(NEVERD_TEST_CLANG, {"-O1", "-w", IR.string(), Reference.string(),
                                 Driver.string(), "-o", Program.string()});
    ASSERT_EQ(Built.exitCode, 0) << Built.err;
    EXPECT_EQ(exec(Program.string(), {}).exitCode, 0);
  }
};

// Six register arguments and four pushed afresh on every iteration of a
// loop, whose stack pointer is a PHI at the loop head.
TEST_F(X86_64_CallAbi, StackArgumentsPushedInALoopReachTheCallee) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
static unsigned ma(unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
                   unsigned, unsigned, unsigned, unsigned)
    __attribute__((noinline));
int manyarg(int a) {
  unsigned s = (unsigned)a, acc = 0x811C9DC5u;
  for (int k = 0; k < 40; k++) {
    unsigned r = ma(s, s ^ acc, s + (unsigned)k, acc, s * 3u, acc >> 2,
                    s ^ 0x9E3779B9u, acc * 7u, s + acc, (unsigned)k * 131u);
    acc = (acc ^ r) * 16777619u;
    s = s * 1664525u + 1013904223u;
  }
  return (int)acc;
}
static unsigned ma(unsigned a, unsigned b, unsigned c, unsigned d, unsigned e,
                   unsigned f, unsigned g, unsigned h, unsigned i,
                   unsigned j) {
  return ((a * 131u + b) ^ (c << 3)) + (d - e) + (f ^ g) + (h * 5u) - i +
         (j >> 1);
}
)C",
                            "manyarg", {"manyarg", "ma"}, {-3, 0, 1, 77});
}

// `cmove` picks which of two stack slots the tail call loads its callee
// from, so the stores that fill both slots are read through that choice.
TEST_F(X86_64_CallAbi, ACalleePickedFromTwoStackSlotsIsTheStoredOne) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
static int pick_p(int) __attribute__((noinline));
static int pick_q(int) __attribute__((noinline));
typedef int (*pick_fn)(int);
int pick(int a) {
  volatile pick_fn s0 = pick_p, s1 = pick_q;
  pick_fn fp = (a & 1) ? s1 : s0;
  return fp(a * 3 + 1);
}
static int pick_p(int x) {
  unsigned h = (unsigned)x;
  for (unsigned i = 0; i < 6; i++)
    h = h * 131u + i;
  return (int)h;
}
static int pick_q(int x) {
  return (int)(((unsigned)x * 2654435761u) ^ ((unsigned)x >> 7));
}
)C",
                            "pick", {"pick", "pick_p", "pick_q"},
                            {-3, 0, 1, 2, 77});
}

// `long f(long a) { return (signed char)a; }` sign-extends the low byte of
// whatever register its caller passes.  Built apart from its callers, its C
// must do the same: a `signed char` parameter would let Clang take the
// caller's extension as done and return 0x1FE whole.
TEST_F(X86_64_CallAbi, AByteReadOfAParameterTakesTheWholeRegister) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "cross-target compilation requires Clang";
  const auto Source = tmpFile("sext8.c");
  const auto Object = tmpFile("sext8.o");
  std::ofstream(Source) << "long sext8(long a) { return (signed char)a; }\n";
  const auto Compiled =
      exec(NEVERD_TEST_CLANG, {"-target", "x86_64-linux-gnu", "-O2", "-c",
                               Source.string(), "-o", Object.string()});
  ASSERT_EQ(Compiled.exitCode, 0) << Compiled.err;
  const auto Decompiled = decompileToHighC(Object);
  ASSERT_EQ(Decompiled.exitCode, 0) << Decompiled.err;
  std::ifstream Input(tmpFile("decompiled_high.c"));
  const std::string C((std::istreambuf_iterator<char>(Input)),
                      std::istreambuf_iterator<char>());

  const auto HighC = tmpFile("sext8-highc.c");
  const auto HighCObject = tmpFile("sext8-highc.o");
  const auto Driver = tmpFile("sext8-driver.c");
  const auto Program = tmpFile("sext8.exe");
  std::ofstream(HighC) << C;
  std::ofstream(Driver) << "long sext8(long);\n"
                           "int main(void) {\n"
                           "  return sext8(0x1FE) == -2 && sext8(0x17F) == 127 "
                           "? 0 : 1;\n"
                           "}\n";
  const auto BuiltC = exec(NEVERD_TEST_CLANG, {"-O2", "-c", HighC.string(),
                                               "-o", HighCObject.string()});
  ASSERT_EQ(BuiltC.exitCode, 0) << BuiltC.err << "\n" << C;
  const auto Linked =
      exec(NEVERD_TEST_CLANG, {"-O2", Driver.string(), HighCObject.string(),
                               "-o", Program.string()});
  ASSERT_EQ(Linked.exitCode, 0) << Linked.err;
  EXPECT_EQ(exec(Program.string(), {}).exitCode, 0) << C;
}

// Ten doubles: eight in xmm0-xmm7 and two on the stack.  The callee's lift
// also lists the six integer argument registers, which it never reads; its
// definition leaves them out, and so must its prototype and its calls.
TEST_F(X86_64_CallAbi, AStackDoubleCalleeIsDeclaredAsItIsDefined) {
  expectHighCRunsLikeSource(
      {"-target", "x86_64-linux-gnu", "-ffp-contract=off"},
      R"C(
static double d10(double, double, double, double, double, double, double,
                  double, double, double) __attribute__((noinline));
int sumd10(int a) {
  double b = (double)(a & 0xffff);
  return (int)d10(b, b + 1, b + 2, b + 3, b + 4, b + 5, b + 6, b + 7, b + 8,
                  b + 9);
}
static double d10(double a, double b, double c, double d, double e, double f,
                  double g, double h, double i, double j) {
  return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9 +
         j * 10;
}
)C",
      "sumd10", {"sumd10", "d10"}, {0, 1, 0x41, 0x1234});
}

// A 32-byte record passes on the stack, and the caller copies it there with
// two 16-byte stores; the callee reads each field with a floating-point
// memory operand.  The call passes one argument per eight-byte slot.
TEST_F(X86_64_CallAbi, ARecordCopiedToTheStackPassesEverySlot) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
typedef struct {
  double a, b, c, d;
} H4;
static double sh4(H4 s, double e) __attribute__((noinline));
int hfa4d(int x) {
  H4 s;
  s.a = (double)(x & 0xff);
  s.b = (double)((x >> 8) & 0xff);
  s.c = (double)(x * 3);
  s.d = (double)(x - 7);
  return (int)sh4(s, (double)(x ^ 5));
}
static double sh4(H4 s, double e) {
  return s.a + 2 * s.b + 3 * s.c + 4 * s.d + 5 * e;
}
)C",
                            "hfa4d", {"hfa4d", "sh4", "H4"},
                            {0, 1, -1, 0x10, 0x1234, 1000});
}

// `cvtsi2sd` writes xmm0's low double and keeps its incoming upper lane,
// and `unpcklpd xmm1, xmm0` reads only the low double of each operand: the
// function takes no vector argument, only its int.
TEST_F(X86_64_CallAbi, ABroadcastOfAConvertedIntTakesNoVectorArgument) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
typedef struct {
  double a, b, c, d;
} B4;
static double sb4(B4 s, double e) __attribute__((noinline));
int bcast(int x) {
  double v = (double)x;
  B4 s;
  s.a = v;
  s.b = v + 1;
  s.c = v + 2;
  s.d = v + 3;
  return (int)sb4(s, v + 4);
}
static double sb4(B4 s, double e) {
  return s.a + 2 * s.b + 3 * s.c + 4 * s.d + 5 * e;
}
)C",
                            "bcast", {"bcast", "sb4", "B4"},
                            {0, 1, -1, 0x10, 77, 1000});
}

// A pointer walking a string is compared against its end through `~W`, the
// bitwise complement of the string's address: the operator takes the address
// as the integer it is, never the array.
TEST_F(X86_64_CallAbi, AComplementOfAStringAddressIsAnInteger) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
static const char W[] = "the_quick_brown_fox_jumps_over_the_lazy_dog_0123";
int strroll(int a) {
  unsigned s = (unsigned)a, h = 0;
  const int N = (int)(sizeof(W) - 1);
  for (int it = 0; it < 16; it++) {
    s = s * 1103515245u + 12345u;
    const char *p = W + ((s >> 6) % (unsigned)N);
    unsigned r = 0;
    for (int k = 0; k < 12; k++) {
      r = r * 31u + (unsigned)(unsigned char)*p;
      p++;
      if (!*p)
        p = W;
    }
    h = h * 131u + r;
    h ^= h >> 10;
  }
  return (int)h;
}
)C",
                            "strroll", {"strroll", "W"}, {0, 1, -1, 0x44, 77});
}

// A System V variadic function tests AL and saves the argument registers to
// its register save area, which va_arg reads.  It takes every one of them as
// a parameter: the integer registers and the vector registers' doubles.
TEST_F(X86_64_CallAbi, AVariadicCalleeTakesItsIntegerArguments) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
#include <stdarg.h>
static int si(int n, ...) __attribute__((noinline));
int vsum_i(int a) {
  unsigned u = (unsigned)a;
  return si(4, (int)u, (int)(u * 3u + 1u), (int)(u ^ 0x55u), (int)(u + 7u));
}
static int si(int n, ...) {
  va_list ap;
  va_start(ap, n);
  int s = 0;
  for (int i = 0; i < n; i++)
    s = s * 31 + va_arg(ap, int);
  va_end(ap);
  return s;
}
)C",
                            "vsum_i", {"vsum_i", "si"}, {0, 1, -1, 0x41, 77});
}

static const char *const VariadicDoubles = R"C(
#include <stdarg.h>
static double sd(int n, ...) __attribute__((noinline));
int vsum_d(int a) {
  double x = (double)a;
  return (int)sd(3, x, x * 2.0, x + 0.5);
}
static double sd(int n, ...) {
  va_list ap;
  va_start(ap, n);
  double s = 0;
  for (int i = 0; i < n; i++)
    s = s * 3.0 + va_arg(ap, double);
  va_end(ap);
  return s;
}
)C";

// The floating arguments of a variadic call travel in the vector registers,
// which the callee saves when AL says the caller set any.
TEST_F(X86_64_CallAbi, AVariadicCalleeTakesItsDoubleArguments) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, VariadicDoubles,
                            "vsum_d", {"vsum_d", "sd"}, {0, 1, -1, 7, 1000});
}

// The LLVM route reads the same parameters: its lifted callee reads the
// doubles a caller passes.
TEST_F(X86_64_CallAbi, ALiftedVariadicCalleeTakesItsDoubleArguments) {
  expectLiftedRunsLikeSource(VariadicDoubles, "vsum_d", "sd");
}

static const char *const VariadicStackIntegers = R"C(
#include <stdarg.h>
static int sb(int n, ...) __attribute__((noinline));
int vsum_big(int a) {
  unsigned u = (unsigned)a;
  return sb(8, (int)u, (int)(u + 1u), (int)(u + 2u), (int)(u + 3u),
            (int)(u + 4u), (int)(u + 5u), (int)(u + 6u), (int)(u + 7u)) ^
         sb(2, (int)(u * 3u), (int)(u ^ 9u));
}
static int sb(int n, ...) {
  va_list ap;
  va_start(ap, n);
  int s = 0;
  for (int i = 0; i < n; i++)
    s = s * 131 + va_arg(ap, int);
  va_end(ap);
  return s;
}
)C";

// Past the six integer registers, va_arg reads its arguments from the stack
// above the return address.  The callee takes each slot its widest call
// stores there as a parameter after every register one, and a call that
// stores fewer passes zero for the rest.
TEST_F(X86_64_CallAbi, AVariadicCalleeTakesItsStackArguments) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"},
                            VariadicStackIntegers, "vsum_big",
                            {"vsum_big", "sb"}, {0, 1, -1, 0x9b, 77});
}

// A lifted call passes those slots after zero for every vector register its
// caller left unset, as the callee takes them in that order.
TEST_F(X86_64_CallAbi, ALiftedVariadicCalleeTakesItsStackArguments) {
  expectLiftedRunsLikeSource(VariadicStackIntegers, "vsum_big", "sb");
}
