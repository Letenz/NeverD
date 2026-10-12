//===- X86_64_StructReturnTests.cpp - Multi-register returns in HighC -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// System V x86-64 returns a small struct in up to two registers per class.
// The callee returns the record of every field, and the caller reads each
// one.  Each kernel is decompiled to HighC and run on the host against its
// own C.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

class X86_64_StructReturn : public HighCHostExecutionTest {};

// Two doubles return in xmm0 and xmm1; the second is no zero.
TEST_F(X86_64_StructReturn, TwoDoublesReturnBoth) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
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
                            "retdd", {"retdd", "mkdd", "dd"}, {-3, 0, 1, 77});
}

// A double and a long return in xmm0 and rax.  The record of the registers
// mixes the classes and lists rax first, the reverse of the source members.
TEST_F(X86_64_StructReturn, ADoubleAndALongReturnBoth) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
typedef struct { double d; long l; } dl;
dl mkdl(int x) __attribute__((noinline));
int retdl(int x) {
  dl r = mkdl(x);
  return (int)r.d ^ (int)(r.l * 7);
}
dl mkdl(int x) {
  dl r;
  r.d = (double)(x >> 3) - 0.5;
  r.l = (long)x * 3 + 1;
  return r;
}
)C",
                            "retdl", {"retdl", "mkdl", "dl"}, {-3, 0, 1, 77});
}

// An int and a double return in rax and xmm0, in the source members' order.
TEST_F(X86_64_StructReturn, AnIntAndADoubleReturnBoth) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
typedef struct { int i; double d; } id;
id mkid(int x) __attribute__((noinline));
int retid(int x) {
  id r = mkid(x);
  return r.i * 5 ^ (int)r.d;
}
id mkid(int x) {
  id r;
  r.i = x * 9 - 4;
  r.d = (double)(x >> 1) + 2.5;
  return r;
}
)C",
                            "retid", {"retid", "mkid", "id"}, {-3, 0, 1, 77});
}

// Two floats share xmm0's low eightbyte.  The callee returns the register's
// whole vector, and the caller reads both lanes of it.
TEST_F(X86_64_StructReturn, TwoFloatsReturnPackedInOneRegister) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
typedef struct { float a, b; } ff;
ff mkff(int x) __attribute__((noinline));
int retff(int x) {
  ff r = mkff(x);
  return (int)r.a ^ ((int)r.b * 5);
}
ff mkff(int x) {
  ff r;
  r.a = (float)(x >> 3);
  r.b = (float)(x >> 1) + 1.0f;
  return r;
}
)C",
                            "retff", {"retff", "mkff", "ff"},
                            {-3, 0, 1, 77, 0x34567});
}

// A vector returns whole in xmm0.
TEST_F(X86_64_StructReturn, AVectorReturnsAllItsLanes) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
typedef unsigned v4su __attribute__((vector_size(16)));
v4su mk4(unsigned x) __attribute__((noinline));
int use4(int x) {
  v4su v = mk4((unsigned)x);
  return (int)(v[0] ^ v[3] * 3u ^ v[1] << 2);
}
v4su mk4(unsigned x) {
  v4su v = {x, x + 1u, x, x * 7u};
  return v + v;
}
)C",
                            "use4", {"use4", "mk4", "v4su"},
                            {-3, 0, 1, 77, 0x12345678});
}
