#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

@interface NDReadOnlyTables : NSObject
- (uint64_t)actionForKind:(uint32_t)kind;
- (int16_t)shortForKind:(uint32_t)kind;
- (uint32_t)maskedForKind:(uint32_t)kind;
- (double)doubleForKind:(uint32_t)kind;
@end
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NDReadOnlyTables *driver = [NDReadOnlyTables new];
    const uint64_t actions[] = {
        UINT64_C(0x1020304050607080), UINT64_C(0xfedcba9876543210),
        UINT64_C(0x0706050403020100), UINT64_C(0x80ff007faa55cc33),
        UINT64_C(0x3141592653589793), UINT64_C(0xffff0000ffff0000),
        UINT64_C(0x8000000000000001), UINT64_C(0x13579bdf02468ace),
        UINT64_C(0xf0e0d0c0b0a09080)};
    const int16_t shorts[] = {-32768, 93,  -70,   8191,  -119, 2038,
                              41,     -21, 30000, 32767, -19,  123,
                              -512,   0,   711,   -83,   98,   -1023};
    const uint32_t masked[] = {91, 1231, 511, 73, 999, 4161, 113, 801};
    const double doubles[] = {0.25, -13.5, 1000.125, -0.0, 17.75};
    uint32_t state = UINT32_MAX;
    unsigned cases = 0;
    for (unsigned i = 0; i < 8192; ++i) {
      state = state * UINT32_C(1664525) + UINT32_C(1013904223);
      const uint32_t edges[] = {0,  1,  2,  3,          11,
                                12, 17, 18, UINT32_MAX, UINT32_C(0x80000000)};
      uint32_t kind = i < 4096 ? i % 32 : state;
      if (i < sizeof(edges) / sizeof(edges[0]))
        kind = edges[i];
      const uint64_t expectedAction =
          kind >= 3 && kind <= 11 ? actions[kind - 3] : 2;
      const int16_t expectedShort = kind < 18 ? shorts[kind] : 1;
      const uint32_t expectedMasked = masked[kind % 8];
      const double expectedDouble = kind < 5 ? doubles[kind] : 4.0;
      const double actualDouble = [driver doubleForKind:kind];
      uint64_t expectedBits, actualBits;
      memcpy(&expectedBits, &expectedDouble, sizeof(expectedBits));
      memcpy(&actualBits, &actualDouble, sizeof(actualBits));
      if ([driver actionForKind:kind] != expectedAction ||
          [driver shortForKind:kind] != expectedShort ||
          [driver maskedForKind:kind] != expectedMasked ||
          actualBits != expectedBits) {
        fprintf(stderr, "read-only table mismatch at kind %u\n", kind);
        abort();
      }
      cases += 4;
    }
    [driver release];
    if (cases != 32768)
      abort();
    puts("read-only-table-cases=32768\ninteger-bits=pass\nfloating-bits="
         "pass\nindex-bounds=pass");
  }
}
