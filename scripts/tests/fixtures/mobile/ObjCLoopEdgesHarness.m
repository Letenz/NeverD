#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

@interface NDLoopEdges : NSObject
- (NSUInteger)fold:(NSUInteger)count
              seed:(NSUInteger)seed
            output:(NSUInteger *)output;
@end
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif
int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NDLoopEdges *driver = [NDLoopEdges new];
    NSUInteger cases = 0;
    for (NSUInteger round = 0; round < 256; ++round) {
      const NSUInteger seed = round * UINT64_C(0x9e3779b97f4a7c15);
      for (NSUInteger input = 0; input < 256; ++input) {
        const NSUInteger count = round & 1 ? NSUIntegerMax - input : input;
        const NSUInteger steps = count & 31;
        NSUInteger expected = seed;
        NSUInteger remaining = steps;
        for (NSUInteger index = 0; index < steps; ++index) {
          const NSUInteger left = steps - index;
          const BOOL odd = expected & 1;
          expected = odd ? expected + left * 3 : (expected << 1) ^ (left + 7);
          remaining = left - 1;
          if (odd && remaining == 2)
            break;
        }
        NSUInteger output = ~seed;
        const NSUInteger result = [driver fold:count seed:seed output:&output];
        if (result != expected + remaining || output != expected) {
          fprintf(stderr, "loop edge mismatch at round %lu input %lu\n",
                  (unsigned long)round, (unsigned long)input);
          abort();
        }
        ++cases;
      }
    }
    [driver release];
    if (cases != 65536)
      abort();
    puts("loop-cases=65536\nreturns-and-stores=pass");
  }
}
