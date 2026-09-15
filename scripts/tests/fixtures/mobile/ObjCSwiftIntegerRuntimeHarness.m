#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static unsigned destroyed;
@interface NDLifetime : NSObject
@end
@implementation NDLifetime
- (void)dealloc {
  ++destroyed;
  [super dealloc];
}
@end
@interface NDSwiftIntegerRuntime : NSObject
- (void *)retainObject:(id)object times:(uint32_t)count;
- (uint8_t)object:(id)object canCastToClass:(Class)target;
@end
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif
int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NDSwiftIntegerRuntime *driver = [NDSwiftIntegerRuntime new];
    for (unsigned i = 0; i < 1024; ++i) {
      NDLifetime *object = [NDLifetime new];
      unsigned count = i % 32;
      if ([driver retainObject:object times:count] != object)
        abort();
      if ([driver object:object canCastToClass:[NSObject class]] != 1 ||
          [driver object:object canCastToClass:[NDLifetime class]] != 1 ||
          [driver object:object canCastToClass:[NSString class]] != 0)
        abort();
      for (unsigned n = 0; n < count; ++n)
        [object release];
      if (destroyed != i)
        abort();
      [object release];
      if (destroyed != i + 1)
        abort();
    }
    [driver release];
    puts(
        "runtime-integer-cases=4096\ncast-results=pass\nreference-counts=pass");
  }
}
