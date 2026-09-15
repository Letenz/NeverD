#import "ObjCInvariantLoops.h"

#import <objc/runtime.h>
#include <stdio.h>
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

// Deliberately override the queried operations so eliminating a class check or
// a getter changes an independently observed effect, even for equal values.
@interface NDInvariantProbe : NSObject {
@public
  NSUInteger value;
  NSUInteger checks;
  NSUInteger reads;
}
@end
@implementation NDInvariantProbe
- (BOOL)isKindOfClass:(Class)cls {
  ++checks;
  return cls == [NSNumber class] || cls == [NSString class];
}
- (NSUInteger)unsignedIntegerValue {
  ++reads;
  return value;
}
- (NSUInteger)length {
  ++reads;
  return value;
}
@end

int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NDInvariantLoops *driver = [NDInvariantLoops new];
    for (NSUInteger round = 0; round < 512; ++round) {
      @autoreleasepool {
        NDInvariantProbe *probe = [NDInvariantProbe new];
        probe->value = UINT64_C(0x7fffffffffffe000) ^ round;
        NSArray *numbers = @[
          @[ @(round), probe, [NSNull null] ], @[], @[ probe, @(round + 1) ]
        ];
        NSArray *strings =
            @[ @[ @"αβ", probe ], @[], @[ [NSNull null], @"abc", probe ] ];
        NSUInteger expectedNumbers = 2 * probe->value + 2 * round + 1;
        NSUInteger expectedStrings = 2 * probe->value + 5;
        if ([driver nestedNumbers:numbers] != expectedNumbers ||
            [driver nestedStrings:strings] != expectedStrings ||
            [driver nestedNumbers:nil] != 0 ||
            [driver nestedStrings:@[ @[], @[] ]] != 0 || probe->checks != 4 ||
            probe->reads != 4)
          return 1;
        [probe release];
      }
    }
    [driver release];
  }
  puts("loop-method-cases=2048\nprobe-checks=2048\nprobe-reads=2048");
  return 0;
}
