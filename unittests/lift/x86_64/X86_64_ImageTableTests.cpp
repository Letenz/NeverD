//===- X86_64_ImageTableTests.cpp - Image tables in HighC -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A table in the image prints as the object the output defines, and an index
// into it keeps the value the machine computed.  Each kernel is decompiled to
// HighC and run on the host against its own C.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

class X86_64_ImageTable : public HighCHostExecutionTest {
protected:
  void expectRunsLikeSource(const std::string &Kernel, const std::string &Entry,
                            std::initializer_list<const char *> Names) {
    expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, Kernel, Entry,
                              Names, {-3, 0, 1, 77});
  }
};

// A call through a table of functions reads the table the output defines:
// the table's address added to an index is no offset into an object.
TEST_F(X86_64_ImageTable, ACallThroughAFunctionTableReadsTheTable) {
  expectRunsLikeSource(R"C(
typedef unsigned (*vf_t)(unsigned);
static unsigned h0(unsigned x) { return (x * 2654435761u) ^ (x >> 15); }
static unsigned h1(unsigned x) { return x + 0x85EBCA6Bu; }
static unsigned h2(unsigned x) { return (x << 13) | (x >> 19); }
struct obj { vf_t f; unsigned k; };
struct obj table[3] = {{h0, 3u}, {h1, 5u}, {h2, 7u}};
int dispatch(int a) {
  unsigned acc = (unsigned)a;
  for (int i = 0; i < 66; i++) {
    const struct obj *o = &table[(acc >> 2) % 3u];
    acc = o->f(acc) + o->k * (unsigned)i;
  }
  return (int)acc;
}
)C",
                       "dispatch", {"dispatch", "table", "h0", "h1", "h2"});
}

// Clang turns the bit loop into a table lookup per byte.  The first index is
// the hash's low byte, and each later one a byte XOR whose operand the table
// yields: neither is the wider value C would compute from what is under the
// extension.
TEST_F(X86_64_ImageTable, ATableIndexKeepsItsWidth) {
  expectRunsLikeSource(R"C(
int crc8(int a) {
  unsigned x = (unsigned)a * 2654435761u;
  unsigned char crc = 0;
  for (int i = 0; i < 4; i++) {
    crc ^= (unsigned char)(x >> (i * 8));
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x80) ? (unsigned char)((crc << 1) ^ 0x07)
                         : (unsigned char)(crc << 1);
  }
  return crc;
}
)C",
                       "crc8", {"crc8"});
}

// A pointer walking a static array is compared with the address one past the
// array's end, where no object starts: that is the array's address plus its
// size, not the number the image gives it.
TEST_F(X86_64_ImageTable, APointerComparesWithTheEndOfItsArray) {
  expectRunsLikeSource(R"C(
static int A[16];
int ptrcmp(int a) {
  unsigned s = (unsigned)a | 1u;
  int h = 0;
  for (int i = 0; i < 16; i++) {
    s = s * 1103515245u + 12345u;
    A[i] = (int)(s >> 9);
  }
  int *p = A + ((s >> 4) & 15u);
  for (int i = 0; i < 200; i++) {
    s = s * 1103515245u + 12345u;
    *p += (int)(s >> 11);
    h = h * 131 + *p;
    p++;
    if (p >= A + 16)
      p = A;
  }
  return h + A[0] + A[15];
}
)C",
                       "ptrcmp", {"ptrcmp", "A"});
}

// A table of functions the code reaches only through a selected base
// (`T = c ? FA : FB; T[i]`) holds the functions' addresses, not their
// offsets in the object's text.
TEST_F(X86_64_ImageTable, ASelectedTableOfFunctionsHoldsTheirAddresses) {
  expectRunsLikeSource(
      R"C(
static int a0(int x) { return x + 11; }
static int a1(int x) { return x * 5; }
static int a2(int x) { return x ^ 0x3c; }
static int a3(int x) { return (x << 2) + 3; }
static int b0(int x) { return x - 7; }
static int b1(int x) { return x * 9 + 2; }
static int b2(int x) { return (x >> 2) ^ x; }
static int b3(int x) { return x * x - 1; }
static int (*const FA[4])(int) = {a0, a1, a2, a3};
static int (*const FB[4])(int) = {b0, b1, b2, b3};
int fnsel(int a) {
  unsigned s = (unsigned)a, h = 0;
  for (int i = 0; i < 200; i++) {
    s = s * 1103515245u + 12345u;
    int (*const *T)(int) = ((s >> 4) & 1u) ? FA : FB;
    int (*f)(int) = T[(s >> 7) & 3u];
    h = h * 131u + (unsigned)f((int)(s >> 3));
    h ^= h >> 10;
  }
  return (int)h;
}
)C",
      "fnsel",
      {"fnsel", "FA", "FB", "a0", "a1", "a2", "a3", "b0", "b1", "b2", "b3"});
}

// At -O0 a local label table is copied from its read-only initializer, an
// object no symbol names: the copy reads the object the output declares,
// not the address the image gives it.
TEST_F(X86_64_ImageTable, AnUnnamedInitializerIsDeclared) {
  KernelOptimization = "-O0";
  expectRunsLikeSource(R"C(
int loc8(int a) {
  const void *tab[8] = {&&L0, &&L1, &&L2, &&L3, &&L4, &&L5, &&L6, &&L7};
  unsigned acc = (unsigned)a;
  int pc = 0;
  for (int i = 0; i < 80; i++) {
    goto *tab[pc & 7];
  L0:
    acc += 0x9E3779B9u;
    pc = (int)(acc & 7);
    continue;
  L1:
    acc ^= (acc << 13) | (acc >> 19);
    pc = (int)((acc >> 7) & 7);
    continue;
  L2:
    acc *= 2654435761u;
    pc = (int)((acc >> 11) & 7);
    continue;
  L3:
    acc -= (acc >> 5);
    pc = (int)((acc >> 3) & 7);
    continue;
  L4:
    acc += 0x85EBCA6Bu;
    pc = (int)((acc >> 2) & 7);
    continue;
  L5:
    acc ^= acc >> 15;
    pc = (int)((acc >> 1) & 7);
    continue;
  L6:
    acc = (acc << 5) | (acc >> 27);
    pc = (int)((acc >> 9) & 7);
    continue;
  L7:
    acc += acc << 1;
    pc = (int)((acc >> 4) & 7);
    continue;
  }
  return (int)acc;
}
)C",
                       "loc8", {"loc8"});
}
