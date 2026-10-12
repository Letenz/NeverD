//===- X86_64_SignedCompareTests.cpp - Signed comparisons in HighC --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A comparison reads its operands as C types the text it prints, not as the
// IR types them.  Each kernel is decompiled to HighC and run on the host
// against its own C.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

class X86_64_SignedCompare : public HighCHostExecutionTest {
protected:
  void expectRunsLikeSource(const std::string &Kernel, const std::string &Entry,
                            std::initializer_list<const char *> Names) {
    expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, Kernel, Entry,
                              Names, {-3, 0, 1, 77});
  }
};

// `(int)(s >> 2) - 0x40000000` prints as an OR with 0xC0000000, which C
// types as an unsigned int, so the signed comparison with the running
// minimum converts it back.
TEST_F(X86_64_SignedCompare, ArithmeticWithAnUnsignedLiteralComparesSigned) {
  expectRunsLikeSource(R"C(
int minmax(int a) {
  int mn = 2000000000, mx = -2000000000, mi = 0, acc = 0;
  unsigned s = (unsigned)a | 1u;
  for (int j = 0; j < 200; j++) {
    int v = (int)(s >> 2) - 0x40000000;
    if (v < mn)
      mn = v;
    if (v > mx) {
      mx = v;
      mi = j;
    }
    acc = acc * 131 + (mn >> 8) + (mx >> 8) + mi;
    s = s * 1103515245u + 12345u;
  }
  return acc ^ mn ^ mx ^ mi;
}
)C",
                       "minmax", {"minmax"});
}

// At -O0 every local lives in a frame slot, declared in one signedness and
// compared in either: `found < 0` tests the int the slot holds.
TEST_F(X86_64_SignedCompare, AFrameSlotComparesAsTheValueItHolds) {
  KernelOptimization = "-O0";
  expectRunsLikeSource(R"C(
int search(int a) {
  unsigned h = (unsigned)a, acc = 0;
  for (int rep = 0; rep < 48; rep++) {
    h = h * 1103515245u + 12345u;
    unsigned tgt = h & 0x3fu;
    int found = -1;
    for (int i = 0; i < 8 && found < 0; i++)
      for (int j = 0; j < 8; j++) {
        unsigned cell = ((unsigned)(i * 8 + j) * 2654435761u) & 0x3fu;
        if (cell == tgt) {
          found = i * 8 + j;
          break;
        }
      }
    acc = acc * 131u + (unsigned)(found + 1);
  }
  return (int)acc;
}
)C",
                       "search", {"search"});
}

// A byte compare-and-swap loop retries until the byte it read is the one the
// swap found.  One side reads as a signed char and the other as an unsigned
// one; C promotes 0xC8 to -56 and to 200, so the bytes compare unsigned.
// The bounded retry ends a loop that would never see them equal.
TEST_F(X86_64_SignedCompare, BytesCompareEqualWhateverTheirSignedness) {
  expectRunsLikeSource(R"C(
static unsigned char gc8;
int acas8(int x) {
  unsigned s = (unsigned)x;
  int out = 0;
  gc8 = 0;
  for (int k = 0; k < 64; k++) {
    s = s * 1103515245u + 12345u;
    unsigned char add = (unsigned char)(s >> 24), oldv, newv;
    int tries = 0;
    do {
      oldv = gc8;
      newv = (unsigned char)(oldv * 3 + add);
    } while (!__sync_bool_compare_and_swap(&gc8, oldv, newv) && ++tries < 4);
    out ^= (int)(unsigned char)gc8;
  }
  return out;
}
)C",
                       "acas8", {"acas8", "gc8"});
}
