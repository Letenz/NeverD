#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
@interface NDRuntimeIvars : NSObject
- (uint64_t)word;
- (void)setWord:(uint64_t)value;
@end
extern void *nd_runtime_ivars_create(uint64_t);
extern int32_t nd_runtime_ivars_check(void *, uint64_t);
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif
int main(void) {
  @autoreleasepool {
    NDRuntimeIvars *driver = (id)nd_runtime_ivars_create(UINT64_MAX);
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    if (!nd_runtime_ivars_check(driver, UINT64_MAX))
      abort();
    uint64_t state = UINT64_MAX;
    unsigned cases = 0;
    for (unsigned i = 0; i < 8192; ++i) {
      state =
          state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
      uint64_t value = i == 0 ? 0 : i == 1 ? UINT64_MAX : state;
      [driver setWord:value];
      if ([driver word] != value || !nd_runtime_ivars_check(driver, value))
        abort();
      cases += 2;
    }
    [driver release];
    if (cases != 16384)
      abort();
    puts("runtime-ivar-cases=16384\nproperty-bits=pass\nresilient-field=pass");
  }
}
