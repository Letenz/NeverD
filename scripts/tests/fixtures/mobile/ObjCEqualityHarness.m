#import "ObjCEquality.h"

#import <objc/runtime.h>
#include <stdio.h>

#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

static unsigned comparisons, destroyed;

@interface NDComparable : NSObject {
@public
  unsigned value;
}
@end

@implementation NDComparable
- (BOOL)isEqual:(id)other {
  ++comparisons;
  return other && value == ((NDComparable *)other)->value;
}
- (void)dealloc {
  ++destroyed;
  [super dealloc];
}
@end

int main(void) {
#ifdef NEVERD_RECOVERED_ARC
  installRecovered();
#endif
  @autoreleasepool {
    NDComparable *values[3];
    for (unsigned i = 0; i != 3; ++i) {
      values[i] = [NDComparable new];
      values[i]->value = i / 2;
    }
    id choices[4] = {nil, values[0], values[1], values[2]};
    for (unsigned repeat = 0; repeat != 16; ++repeat)
      for (unsigned pattern = 0; pattern != 256; ++pattern) {
        @autoreleasepool {
          NDEquality *a = [NDEquality new];
          NDEquality *b = [NDEquality new];
          unsigned indices[4] = {pattern & 3, (pattern >> 2) & 3,
                                 (pattern >> 4) & 3, pattern >> 6};
          a.leftValue = choices[indices[0]];
          b.leftValue = choices[indices[1]];
          a.rightValue = choices[indices[2]];
          b.rightValue = choices[indices[3]];
          // Compute both truth and the short-circuit call count from the
          // indices, independently of the method or Objective-C dispatch.
          BOOL equal[2];
          unsigned calls = 0;
          for (unsigned pair = 0; pair != 2; ++pair) {
            unsigned x = indices[2 * pair], y = indices[2 * pair + 1];
            equal[pair] = x == y || (x && y && (x - 1) / 2 == (y - 1) / 2);
            if ((!pair || equal[0]) && x && x != y)
              ++calls;
          }
          comparisons = 0;
          if ([a equivalent:b] != (equal[0] && equal[1]) ||
              comparisons != calls)
            return 1;
#ifdef NEVERD_STANDALONE_SOURCE
          // The project exporter cannot spell .cxx_destruct as an Objective-C
          // method yet. The C API test installs and executes that destructor;
          // this export check exercises the recovered strong setters instead.
          a.leftValue = a.rightValue = nil;
          b.leftValue = b.rightValue = nil;
#endif
          [a release];
          [b release];
        }
        if (destroyed)
          return 2;
      }
    for (unsigned i = 0; i != 3; ++i)
      [values[i] release];
    if (destroyed != 3)
      return 3;
    puts("equality-cases=4096\nshort-circuit=pass\nownership=pass");
  }
  return 0;
}
