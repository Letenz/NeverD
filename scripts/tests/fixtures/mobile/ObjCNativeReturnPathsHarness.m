#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

@interface NDNativeReturnPaths : NSObject
- (uint64_t)adjusted:(uint64_t)value
              choose:(unsigned)choice
              output:(uint64_t *)output;
- (uint64_t)wideLeaf:(uint64_t)value;
@end
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NDNativeReturnPaths *driver = [NDNativeReturnPaths new];
    const uint64_t edges[] = {0,
                              1,
                              8,
                              9,
                              UINT64_MAX,
                              UINT64_MAX - 7,
                              UINT64_C(1) << 63,
                              (UINT64_C(1) << 63) - 1};
    const unsigned choices[] = {0, 1, 2, UINT32_MAX};
    uint64_t state = 1;
    unsigned cases = 0;
    for (unsigned i = 0; i < 4096; ++i) {
      state = state * UINT64_C(6364136223846793005) + 1;
      const uint64_t input = i < 8 ? edges[i] : state;
      const uint64_t wideExpected = (int64_t)input < 21 ? UINT32_MAX : 7;
      if ([driver wideLeaf:input] != wideExpected)
        abort();
      for (unsigned c = 0; c < 4; ++c) {
        const unsigned choice = choices[c];
        const uint64_t expected = choice ? input + 7 : input - 9;
        uint64_t stored = ~expected;
        const uint64_t result = [driver adjusted:input
                                          choose:choice
                                          output:&stored];
        if (result != expected || stored != expected) {
          fprintf(stderr, "native return mismatch at input %u, choice %u\n", i,
                  choice);
          abort();
        }
        ++cases;
      }
    }
    [driver release];
    if (cases != 16384)
      abort();
    puts("native-return-cases=16384\nreturns-and-stores=pass\nwide-leaf-cases="
         "4096");
  }
}
