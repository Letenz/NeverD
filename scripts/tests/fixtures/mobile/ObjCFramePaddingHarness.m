#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>

@interface NDFramePadding : NSObject
@property(nonatomic) BOOL horizontal;
@property(nonatomic) BOOL vertical;
- (NSString *)key;
@end

static unsigned getterTrace;
@interface NDCountingFramePadding : NDFramePadding
@end
@implementation NDCountingFramePadding
- (BOOL)horizontal {
  getterTrace = getterTrace * 10 + 1;
  return [super horizontal];
}
- (BOOL)vertical {
  getterTrace = getterTrace * 10 + 2;
  return [super vertical];
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
    NDFramePadding *driver = [NDCountingFramePadding new];
    NSString *expected[] = {@"flip-0-0", @"flip-1-0", @"flip-0-1", @"flip-1-1"};
    unsigned cases = 0;
    for (unsigned i = 0; i < 4096; ++i) {
      for (unsigned flags = 0; flags < 4; ++flags) {
        driver.horizontal = (flags & 1) != 0;
        driver.vertical = (flags & 2) != 0;
        getterTrace = 0;
        NSString *key = [driver key];
        if (getterTrace != 12 || ![key isEqualToString:expected[flags]]) {
          fprintf(stderr, "frame padding mismatch at flags %u, trace %u\n",
                  flags, getterTrace);
          abort();
        }
        ++cases;
      }
    }
    [driver release];
    if (cases != 16384)
      abort();
    puts("frame-padding-cases=16384\nscalar-values=pass\ngetter-effects=pass");
  }
}
