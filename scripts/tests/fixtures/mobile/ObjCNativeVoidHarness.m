#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static NSUInteger destructions;
@interface NDTracked : NSObject
@end
@implementation NDTracked
- (void)dealloc { ++destructions; [super dealloc]; }
@end
@interface NDVoidForwarders : NSObject
- (void)releaseObject:(void *)object active:(NSUInteger)active;
- (NSUInteger)releaseObject:(void *)object active:(NSUInteger)active result:(NSUInteger)value;
@end
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif
int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NDVoidForwarders *driver = [NDVoidForwarders new];
    uint64_t random = UINT64_C(0xfedcba9876543210);
    for (unsigned i = 0; i < 8192; ++i) {
      random ^= random << 13; random ^= random >> 7; random ^= random << 17;
      NSUInteger active = i & 1;
      NDTracked *object = [NDTracked new];
      NSUInteger before = destructions;
      [driver releaseObject:object active:active];
      if (destructions != before + active) abort();
      if (!active) [object release];
      active = (i >> 1) & 1;
      object = [NDTracked new]; before = destructions;
      const NSUInteger value = i == 0 ? 0 : i == 1 ? UINT64_MAX : random;
      if ([driver releaseObject:object active:active result:value] != value ||
          destructions != before + active) abort();
      if (!active) [object release];
    }
    if (destructions != 16384) abort();
    [driver release];
    puts("native-void-cases=16384\nrelease-effects=pass\nindependent-results=pass");
  }
}
