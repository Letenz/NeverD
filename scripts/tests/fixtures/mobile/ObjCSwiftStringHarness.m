#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>

@interface NDSwiftString : NSObject
- (NSString *)bridgeWord:(uint64_t)word storage:(void *)storage;
@end
extern int32_t nd_swift_string_case_count(void);
extern void nd_swift_string_words(int32_t, uint64_t *);
extern void *nd_swift_string_expected(int32_t);
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NDSwiftString *box = [NDSwiftString new];
    for (unsigned repeat = 0; repeat != 512; ++repeat) {
      for (int32_t i = 0; i < nd_swift_string_case_count(); ++i) {
        NSString *actual;
        NSString *expected = (NSString *)nd_swift_string_expected(i);
        @autoreleasepool {
          uint64_t words[2];
          nd_swift_string_words(i, words);
          actual = [[box bridgeWord:words[0]
                            storage:(void *)(uintptr_t)words[1]] retain];
          if (![actual isEqualToString:expected])
            return 1;
        }
        // The source method returns an autoreleased owned bridge result. A
        // retained result must remain valid after the caller's pool drains.
        if (![actual isEqualToString:expected])
          return 2;
        [actual release];
        [expected release];
      }
    }
    [box release];
  }
  puts("swift-string=pass\ncontents=pass\nlifetime=pass");
  return 0;
}
