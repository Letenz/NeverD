//===- AArch64_StructReturnTests.cpp - Multi-register returns in HighC ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A small struct returns across several registers: a homogeneous float
// aggregate in v0..v3, any other one of up to sixteen bytes in x0:x1.  The
// callee returns the record of every field, and the caller reads each one.
// Each kernel is decompiled to HighC and run on the host against its own C.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

class AArch64_StructReturn : public HighCHostExecutionTest {
protected:
  void expectRunsLikeSource(const std::string &Kernel, const std::string &Entry,
                            std::initializer_list<const char *> Names) {
    expectHighCRunsLikeSource({"-target", "aarch64-linux-gnu"}, Kernel, Entry,
                              Names, {0, 1, -1, 0x15, 0x27, 0x3F, 0x12345678});
  }
};

// Two doubles return in d0 and d1; the caller reads both.
TEST_F(AArch64_StructReturn, TwoDoublesReturnBoth) {
  expectRunsLikeSource(R"C(
typedef struct { double a, b; } dd;
dd mkdd(int x) __attribute__((noinline));
int retdd(int x) {
  dd r = mkdd(x);
  return (int)r.a ^ ((int)r.b * 9);
}
dd mkdd(int x) {
  dd r;
  r.a = (double)(x >> 2) + 0.25;
  r.b = (double)(x >> 4) - 1.75;
  return r;
}
)C",
                       "retdd", {"retdd", "mkdd", "dd"});
}

// Three floats are twelve bytes in s0..s2, which no integer bit casts from.
TEST_F(AArch64_StructReturn, ThreeFloatsReturnAll) {
  expectRunsLikeSource(R"C(
typedef struct { float a, b, c; } f3;
f3 mkf3(int x) __attribute__((noinline));
int retf3(int x) {
  f3 r = mkf3(x);
  return (int)r.a ^ ((int)r.b * 9) ^ ((int)r.c * 5);
}
f3 mkf3(int x) {
  f3 r;
  r.a = (float)(x >> 2) + 0.25f;
  r.b = (float)(x >> 4) - 1.75f;
  r.c = (float)x * 0.5f;
  return r;
}
)C",
                       "retf3", {"retf3", "mkf3", "f3"});
}

// A double and an int are no float aggregate: they return in x0:x1, the
// double's bits in x0.
TEST_F(AArch64_StructReturn, ADoubleAndAnIntReturnInTheIntegerPair) {
  expectRunsLikeSource(R"C(
typedef struct { double d; int i; } di;
di mkdi(int x) __attribute__((noinline));
int retdi(int x) {
  di r = mkdi(x);
  return (int)r.d ^ (r.i * 9);
}
di mkdi(int x) {
  di r;
  r.d = (double)(x >> 2) + 0.25;
  r.i = x * 3 + 1;
  return r;
}
)C",
                       "retdi", {"retdi", "mkdi", "di"});
}
