#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
@interface NDNativeAuxiliaryInputs : NSObject
- (uint64_t)fillWithWord:(uint64_t)value buffer:(uint64_t *)buffer;
- (uint64_t)addWord:(uint64_t)value buffer:(uint64_t *)buffer;
@end
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif
int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NDNativeAuxiliaryInputs *driver = [NDNativeAuxiliaryInputs new];
    uint64_t state = UINT64_MAX;
    unsigned cases = 0;
    for (unsigned i = 0; i < 8192; ++i) {
      state =
          state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
      uint64_t value = i == 0 ? UINT64_MAX : i == 1 ? 0 : state;
      uint64_t storage[] = {UINT64_C(0x8000000000000001), state, state, state,
                            UINT64_MAX};
      uint64_t *buffer = storage + 1;
      if ([driver fillWithWord:value buffer:buffer] != (~value ^ (value + 7)) ||
          buffer[0] != value || buffer[1] != ~value || buffer[2] != value + 7)
        abort();
      uint64_t increment =
          i < 64 ? UINT64_C(1) << i : state ^ UINT64_C(0x8000000000000001);
      if ([driver addWord:increment
                   buffer:buffer] != ((value + increment) ^ ~value) ||
          buffer[0] != value + increment || buffer[1] != ~value ||
          buffer[2] != value + 7 ||
          storage[0] != UINT64_C(0x8000000000000001) ||
          storage[4] != UINT64_MAX)
        abort();
      cases += 2;
    }
    [driver release];
    if (cases != 16384)
      abort();
    puts("native-auxiliary-cases=16384\nresult-buffers=pass\nscalar-results="
         "pass");
  }
}
