#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

@interface NDSwitchEffects : NSObject
- (NSUInteger)choose:(NSUInteger)selector
              object:(id)value
              output:(NSUInteger *)output;
@end
static NSUInteger *ObservedOutput;
static NSUInteger Seed;
static NSUInteger HashCalls;
@interface NDSwitchObserver : NSObject
@end
@implementation NDSwitchObserver
- (NSUInteger)hash {
  ++HashCalls;
  return *ObservedOutput + Seed + HashCalls * 7;
}
@end
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif
static void checked(int value, unsigned line) {
  if (!value) {
    fprintf(stderr, "switch effect check failed at line %u\n", line);
    abort();
  }
}
#define check(value) checked(!!(value), __LINE__)

int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NDSwitchEffects *driver = [NDSwitchEffects new];
    NDSwitchObserver *observer = [NDSwitchObserver new];
    NSUInteger totalCalls = 0;
    for (NSUInteger round = 0; round < 256; ++round) {
      Seed = round * UINT64_C(0x123456789abcd);
      for (NSUInteger index = 0; index < 22; ++index) {
        const NSUInteger selector = index == 21 ? NSUIntegerMax : index;
        const NSUInteger arm = selector == 16  ? 0
                               : selector < 16 ? selector
                                               : 7;
        const NSUInteger stored = arm * 101 + 17;
        NSUInteger output = NSUIntegerMax;
        ObservedOutput = &output;
        HashCalls = 0;
        id value = round & 1 ? nil : observer;
        const NSUInteger hash = value ? stored + Seed + 7 : 0;
        NSUInteger expected;
        switch (arm % 4) {
        case 0:
          expected = hash + arm + 11;
          break;
        case 1:
          expected = hash ^ (arm + 19);
          break;
        case 2:
          expected = hash * (arm + 3);
          break;
        default:
          expected = (hash << (arm % 5 + 1)) | (arm + 1);
          break;
        }
        check([driver choose:selector object:value output:&output] == expected);
        check(output == stored);
        check(HashCalls == (value ? 1U : 0U));
        totalCalls += HashCalls;
      }
    }
    check(totalCalls == 2816);
    [observer release];
    [driver release];
    puts("switch-cases=5632\nhash-effects=2816\nshared-targets=pass");
  }
}
