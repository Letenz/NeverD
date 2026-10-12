//===- ARM32_ObjectImmediateTests.cpp - Object immediates in HighC --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// An ARM object addresses its own sections only through relocations, of a
// literal word or a MOVW/MOVT pair, so an immediate an instruction no
// relocation writes moves is a number even where an object of the image
// starts at that value.  Each kernel is decompiled to HighC and run on the
// host against its own C.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

class ARM32_ObjectImmediate : public HighCHostExecutionTest {
protected:
  ARM32_ObjectImmediate() { HostFlags = {"-m32"}; }
};

// The trip count 200 equals the address the image gives the last row: it
// stays the count.
TEST_F(ARM32_ObjectImmediate, ALoopCountIsNoAddress) {
  expectHighCRunsLikeSource(
      {"-target", "armv7-linux-gnueabi"}, R"C(
static const int r0[5] = {2, 3, 5, 7, 11};
static const int r1[5] = {13, 17, 19, 23, 29};
static const int r2[5] = {31, 37, 41, 43, 47};
static const int r3[5] = {53, 59, 61, 67, 71};
static const int *const rows[4] = {r0, r1, r2, r3};
int arrind(int a) {
  unsigned s = (unsigned)a, h = 0;
  for (int i = 0; i < 200; i++) {
    s = s * 1103515245u + 12345u;
    const int *p = rows[(s >> 6) & 3u];
    unsigned acc = 0;
    for (int j = 0; j < 5; j++)
      acc = acc * 7u + (unsigned)*p++;
    h = h * 131u + acc;
    h ^= h >> 9;
  }
  return (int)h;
}
)C",
      "arrind", {"arrind", "r0", "r1", "r2", "r3", "rows"}, {-3, 0, 1, 77});
}

// `vcmp.f64 d19, #0` compares with +0.0 at the double's width.  A 4-byte
// zero against the double was no comparison: its flags lowered to unknown
// values, and the C trapped on the selects that read them.
TEST_F(ARM32_ObjectImmediate, ACompareWithZeroIsADoubleCompare) {
  expectHighCRunsLikeSource({"-target", "armv7-linux-gnueabi",
                             "-mfpu=vfpv3-d16", "-mfloat-abi=softfp"},
                            R"C(
int seld(int a) {
  unsigned long long s[6] = {0x7FF8000000000001ULL, 0x7FF0000000000000ULL,
                             0xFFF0000000000000ULL, 0ULL,
                             0x8000000000000000ULL, 0x400921FB54442D18ULL};
  unsigned acc = 0;
  for (int i = 0; i < 6; i++) {
    double x;
    __builtin_memcpy(&x, &s[i], 8);
    for (int j = 0; j < 6; j++) {
      double y;
      __builtin_memcpy(&y, &s[j], 8);
      unsigned c1 = (x < y) ? 3u : 5u;
      unsigned c2 = (x == y) ? 7u : 11u;
      unsigned c3 = (x >= y) ? 13u : 17u;
      acc = acc * 3u + c1 + c2 + c3 + (unsigned)(a + i * j);
    }
  }
  return (int)acc;
}
)C",
                            "seld", {"seld"}, {0, 1, -1, 7, 0x12345678});
}

// `vzip.8 d23, d27` interleaves bytes of d27, which nothing wrote, but each
// 32-bit lane is masked with q9 = 0xff before it is added and clamped.  The
// mask is two 8-byte constants concatenated, and no byte it clears is one any
// result reads: the unknown register is no value of the C.
TEST_F(ARM32_ObjectImmediate, AConcatenatedMaskClearsAnUnknownLane) {
  expectHighCRunsLikeSource({"-target", "armv7-linux-gnueabi",
                             "-mcpu=cortex-a15", "-mfloat-abi=softfp"},
                            R"C(
int satb2(int a, int b) __attribute__((noinline));
int satb(int a) { return satb2(a, a * 7 + 3); }
int satb2(int a, int b) {
  unsigned char x[32], y[32];
  int s = 0;
  for (int i = 0; i < 32; i++) {
    x[i] = (unsigned char)((a * (i + 3)) ^ (i * 11));
    y[i] = (unsigned char)((b * (i + 5)) ^ (i * 13));
  }
  for (int i = 0; i < 32; i++) {
    int v = (int)x[i] + (int)y[i];
    if (v > 255)
      v = 255;
    s += v;
  }
  return s;
}
)C",
                            "satb", {"satb", "satb2"},
                            {0, 1, -1, 7, 0x12345678});
}

// The cursors start at frame addresses and move by a select each iteration.
// `v10 = v9 - 4` once took v9's first value as a frame alias, so the loop
// test compared `frame_base - 24`, the first iteration's address, with the
// other cursor every time.
TEST_F(ARM32_ObjectImmediate, ACursorFromAFrameAddressKeepsMoving) {
  expectHighCRunsLikeSource({"-target", "armv7-linux-gnueabi",
                             "-mcpu=cortex-a15", "-mfloat-abi=softfp"},
                            R"C(
int ptrdiff(int a) {
  unsigned buf[40];
  for (int i = 0; i < 40; i++)
    buf[i] = (unsigned)a + (unsigned)i * 7u;
  unsigned *lo = buf, *hi = buf + 39;
  unsigned h = 0;
  while (lo < hi) {
    long d = hi - lo;
    h += (unsigned)d * 131u + *lo + *hi;
    if (*lo < *hi)
      lo++;
    else
      hi--;
    h ^= (unsigned)(hi - lo);
  }
  return (int)h;
}
)C",
                            "ptrdiff", {"ptrdiff"}, {0, 1, -1, 9, 0x12345678});
}
