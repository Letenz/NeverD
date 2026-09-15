#import "ObjCPredicateFormats.h"

#import <objc/runtime.h>
#include <stdio.h>
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

int main(void) {
#ifdef NEVERD_RECOVERED_ARC
  installRecovered();
#endif
  @autoreleasepool {
    NDPredicateFormats *object = [NDPredicateFormats new];
    for (int i = -1024; i < 1024; ++i) {
      NSPredicate *p = [object object:@(i)];
      if (![p evaluateWithObject:@(i)] || [p evaluateWithObject:@(i + 1)])
        return 1;
      p = [object key:@"value" value:@(i)];
      if (![p evaluateWithObject:@{
            @"value" : @(i)
          }] ||
          [p evaluateWithObject:@{
            @"value" : @(i + 1)
          }])
        return 2;
      NSInteger lower = (NSInteger)i * 1000000000;
      p = [object minimum:lower maximum:lower + 2];
      if (![p evaluateWithObject:@(lower)] ||
          [p evaluateWithObject:@(lower + 2)])
        return 3;
      p = [object count:i];
      if (![p evaluateWithObject:@(i)] || [p evaluateWithObject:@(i - 1)])
        return 4;
      double score = i / 4.0;
      p = [object score:score];
      if (![p evaluateWithObject:@(score + 0.25)] ||
          [p evaluateWithObject:@(score)])
        return 5;
      p = [object quoted:@(i)];
      if (![p evaluateWithObject:@"%@"] || ![p evaluateWithObject:@(i)] ||
          [p evaluateWithObject:@(i + 1)])
        return 6;
      if (![[object always] evaluateWithObject:nil])
        return 7;
      NSExpression *e = [object expression:lower];
      if ([[e expressionValueWithObject:nil
                                context:nil] longLongValue] != lower + 7)
        return 8;
    }
    [object release];
    puts(
        "predicate-checks=30720\nquoted-placeholders=pass\nscalar-widths=pass");
  }
  return 0;
}
