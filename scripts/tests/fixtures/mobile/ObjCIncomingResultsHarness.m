#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
@interface NDIncomingResults : NSObject
- (uint64_t)word:(uint64_t)value flags:(uint64_t)flags;
- (uint64_t)word:(uint64_t)value memory:(const uint64_t *)memory;
@end
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif
int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NDIncomingResults *driver = [NDIncomingResults new];
    uint64_t state = UINT64_MAX;
    unsigned cases = 0;
    for (unsigned i = 0; i < 8192; ++i) {
      state =
          state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
      uint64_t value = i == 0 ? UINT64_MAX : i == 1 ? 0 : state;
      uint64_t flags = i < 32 ? i : state >> 7;
      uint64_t memory = flags ^ UINT64_C(0x8000000000000001);
      uint64_t saved = memory;
      uint64_t expectedWord = value + (flags % 2 ? flags : 0);
      uint64_t expectedMemory = value + (memory % 2 ? memory : 0);
      if ([driver word:value flags:flags] != expectedWord ||
          [driver word:value memory:&memory] != expectedMemory ||
          memory != saved)
        abort();
      cases += 2;
    }
    [driver release];
    if (cases != 16384)
      abort();
    puts("incoming-result-cases=16384\nbit-patterns=pass\nmemory-input=pass");
  }
}
