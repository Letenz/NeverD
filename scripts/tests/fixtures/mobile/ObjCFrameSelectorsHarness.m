#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>

@interface NDFrameSelectors : NSObject
- (NSString *)removing:(NSString *)word from:(NSString *)input;
@end
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif
int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NDFrameSelectors *driver = [NDFrameSelectors new];
    NSArray *tokens = @[ @"", @"a", @"b", @"xx", @"中文" ];
    unsigned cases = 0;
    for (unsigned code = 0; code < 4096; ++code) {
      @autoreleasepool {
        NSMutableArray *parts = [NSMutableArray array];
        unsigned state = code;
        for (unsigned i = 0; i <= code % 9; ++i) {
          [parts addObject:tokens[state % tokens.count]];
          state = state * 1664525U + 1013904223U;
        }
        NSString *input = [[parts componentsJoinedByString:@":"] copy];
        for (NSString *word in tokens) {
          NSMutableArray *kept = [NSMutableArray array];
          for (NSUInteger i = 0; i < parts.count; ++i)
            if (i == 0 || word.length == 0 || ![parts[i] isEqualToString:word])
              [kept addObject:parts[i]];
          NSString *expected = [kept componentsJoinedByString:@":"];
          NSString *result = [driver removing:word from:input];
          if (![result isEqualToString:expected] ||
              (kept.count == parts.count && result != input)) {
            fprintf(stderr, "frame selector mismatch at input %u\n", code);
            abort();
          }
          ++cases;
        }
        [input release];
      }
    }
    [driver release];
    if (cases != 20480)
      abort();
    puts("frame-selector-cases=20480\nstrings-and-identity=pass");
  }
}
