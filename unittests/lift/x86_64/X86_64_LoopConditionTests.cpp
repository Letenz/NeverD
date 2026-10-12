//===- X86_64_LoopConditionTests.cpp - Loop conditions in HighC -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A condition inside a loop reads the values the loop carries.  Each kernel
// is decompiled to HighC and run on the host against its own C.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

class X86_64_LoopCondition : public HighCHostExecutionTest {};

// `jbe` after a `je` reads the flags of the same `cmp`.  Dropping the equal
// case from it, which the `je` already left, keeps the key and the probed
// entry of the iteration: the values assigned before the loops are only
// those of the first one.
TEST_F(X86_64_LoopCondition, ATestAfterAnEqualOneReadsTheIterationsValues) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
static const unsigned char keys[32] = {
    2,   9,   15,  22,  28,  35,  41,  48,  54,  61,  67,
    74,  80,  87,  93,  100, 106, 113, 119, 126, 132, 139,
    145, 152, 158, 165, 171, 178, 184, 191, 197, 204};
static const unsigned char vals[32] = {
    17, 3,  28, 9,  41, 6, 33, 52, 11, 46, 7,  38, 23, 60, 14, 49,
    5,  31, 57, 12, 44, 8, 26, 53, 19, 40, 2,  35, 58, 21, 47, 10};
int binsrch(int a) {
  unsigned s = (unsigned)a, out = 0;
  for (int it = 0; it < 128; it++) {
    s = s * 1103515245u + 12345u;
    unsigned acc = s;
    for (int q = 0; q < 32; q++) {
      unsigned key = (s >> (q & 15)) & 0xFFu;
      int lo = 0, hi = 31;
      unsigned found = 0;
      while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        unsigned mv = keys[mid];
        if (mv == key) {
          found = vals[mid] + 1u;
          break;
        } else if (mv < key)
          lo = mid + 1;
        else
          hi = mid - 1;
      }
      acc = acc * 131u + found + (unsigned)lo;
    }
    out = out * 1311u + acc;
  }
  return (int)out;
}
)C",
                            "binsrch", {"binsrch", "keys", "vals"},
                            {0x14, 0, 1, -1, 12345});
}
