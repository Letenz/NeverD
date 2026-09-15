#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

@interface NDConstantObjects : NSObject
- (NSArray *)words;
- (NSArray *)nested;
- (NSNumber *)signedNumber;
- (NSNumber *)unsignedNumber;
- (NSDictionary *)mapping;
@end
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

static void *check(void *context) {
  @autoreleasepool {
    NDConstantObjects *driver = context;
    for (unsigned i = 0; i < 4096; ++i) {
      NSArray *words = [driver words];
      NSArray *nested = [driver nested];
      NSNumber *negative = [driver signedNumber];
      NSNumber *positive = [driver unsignedNumber];
      NSDictionary *mapping = [driver mapping];
      NSArray *inner = nested[0];
      NSArray *mapped = mapping[@"first"];
      if (![words isEqual:@[ @"alpha", @"βeta", @"alpha" ]] ||
          ![nested
              isEqual:@[ @[ @"alpha", @"βeta" ], @[ @"tail" ], @(-12345LL) ]] ||
          mapping.count != 2 || ![mapped isEqual:inner] ||
          negative.longLongValue != -12345 || strcmp(negative.objCType, "q") ||
          positive.unsignedLongLongValue != UINT64_C(0xfedcba9876543210) ||
          strcmp(positive.objCType, "Q") || words[0] != words[2] ||
          words[0] != inner[0] || words[1] != inner[1] ||
          words[0] != mapped[0] || nested[2] != negative ||
          mapping[@"second"] != negative || [driver words] != words ||
          [driver nested] != nested || [driver mapping] != mapping ||
          [driver signedNumber] != negative ||
          [driver unsignedNumber] != positive)
        return (void *)1;
      for (id value in @[ words, nested, mapping, negative, positive ]) {
        id copied = [value copy];
        if (copied != value)
          return (void *)2;
        [copied release];
      }
    }
  }
  return 0;
}

int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NDConstantObjects *driver = [NDConstantObjects new];
    pthread_t threads[8];
    for (unsigned i = 0; i < 8; ++i)
      if (pthread_create(&threads[i], 0, check, driver))
        abort();
    for (unsigned i = 0; i < 8; ++i) {
      void *result = 0;
      if (pthread_join(threads[i], &result) || result)
        abort();
    }
    [driver release];
    puts("constant-object-checks=32768\ncontents-and-aliases=pass\nconcurrent-"
         "initialization=pass");
  }
}
