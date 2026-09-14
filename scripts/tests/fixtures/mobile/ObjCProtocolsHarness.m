#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdio.h>

@interface NDProtocolCalls : NSObject
- (NSUInteger)enumerate:(id<NSFastEnumeration>)source
                  state:(NSFastEnumerationState *)state
                objects:(id __unsafe_unretained *)objects
                  count:(NSUInteger)count;
- (uint64_t)metricOf:(id)source;
- (BOOL)isNegativeMetric:(id)source;
@end

@interface NDMetricProvider : NSObject {
@public
  uint64_t bits;
}
- (int64_t)metric;
@end
@implementation NDMetricProvider
- (int64_t)metric {
  return (int64_t)bits;
}
@end

#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NDProtocolCalls *calls = [NDProtocolCalls new];
    NSUInteger visited = 0;
    for (NSUInteger length = 0; length <= 128; ++length) {
      NSMutableArray *source = [NSMutableArray array];
      for (NSUInteger index = 0; index < length; ++index)
        [source addObject:@(index)];
      for (NSUInteger capacity = 1; capacity <= 16; ++capacity) {
        NSFastEnumerationState state = {0};
        id __unsafe_unretained buffer[16];
        NSUInteger total = 0;
        while (true) {
          NSUInteger count = [calls enumerate:source
                                        state:&state
                                      objects:buffer
                                        count:capacity];
          if (count == 0)
            break;
          if (count > length - total || !state.itemsPtr || !state.mutationsPtr)
            return 2;
          for (NSUInteger index = 0; index < count; ++index)
            if (![state.itemsPtr[index] isEqual:@(total + index)])
              return 3;
          total += count;
          if (total > length)
            return 4;
        }
        if (total != length)
          return 5;
        visited += total;
      }
    }
    printf("enumerated=%lu\n", (unsigned long)visited);
    NDMetricProvider *provider = [NDMetricProvider new];
    uint64_t state = 0x931492a5572efd81;
    for (unsigned index = 0; index < 4096; ++index) {
      const uint64_t edges[] = {0, 1, INT64_MAX, UINT64_C(1) << 63, UINT64_MAX};
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
      provider->bits = index < 5 ? edges[index] : state;
      if ([calls metricOf:provider] != provider->bits)
        return 6;
      if (!![calls isNegativeMetric:provider] != !!(provider->bits >> 63))
        return 7;
    }
    [provider release];
    [calls release];
    printf("integer-bits=4096\n");
  }
  return 0;
}
