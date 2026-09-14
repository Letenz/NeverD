#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

@interface NDSwiftStorage : NSObject
- (instancetype)initWithValue:(double)value count:(int64_t)count;
@property(nonatomic, readonly) double value;
@property(nonatomic, readonly) int64_t count;
@end
#ifdef NEVERD_RECOVERED_STORAGE
#include "replacements.h"
#endif

int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_STORAGE
    installRecovered();
#endif
    uint64_t state = UINT64_C(0xabc357802319);
    for (unsigned index = 0; index < 1024; ++index) {
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
      const uint64_t bits = index == 0   ? 0
                            : index == 1 ? UINT64_C(0x8000000000000000)
                            : index == 2 ? UINT64_C(0x7ff8000000000042)
                                         : state;
      double value;
      memcpy(&value, &bits, sizeof(value));
      int64_t count;
      memcpy(&count, &state, sizeof(count));
      NDSwiftStorage *object = [[NDSwiftStorage alloc] initWithValue:value
                                                               count:count];
      const double actual = object.value;
      uint64_t actualBits;
      memcpy(&actualBits, &actual, sizeof(actualBits));
      if (actualBits != bits || object.count != count)
        return 2;
      [object release];
    }
  }
  puts("swift-storage=pass\ngetter-calls=2048");
  return 0;
}
