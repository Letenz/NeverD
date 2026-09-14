#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>

@interface NDConstantStrings : NSObject
- (NSString *)ascii;
- (NSString *)alias;
- (NSString *)unicode;
- (NSString *)embedded;
- (NSString *)empty;
- (NSString *)first;
- (NSString *)second;
@end

#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

static void checked(int value, unsigned line) {
  if (!value) {
    fprintf(stderr, "constant string check failed at line %u\n", line);
    abort();
  }
}
#define check(value) checked(!!(value), __LINE__)

int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NDConstantStrings *calls = [NDConstantStrings new];
    for (unsigned round = 0; round < 128; ++round) {
      check([[calls ascii] isEqualToString:@"sites\n\"quoted\""]);
      check([calls ascii] == [calls alias]);
      check([[calls unicode] isEqualToString:@"百科😀"]);
      check([[calls unicode] length] == 4);
      check([[calls embedded] length] == 3);
      check([[calls embedded] characterAtIndex:1] == 0);
      check([[calls embedded] characterAtIndex:2] == 'b');
      check([[calls empty] length] == 0);
      check([calls first] != [calls second]);
      check([[calls first] isEqual:[calls second]]);
      for (NSString *value in @[
             [calls ascii], [calls unicode], [calls embedded], [calls empty],
             [calls first], [calls second]
           ]) {
        check(object_getClass(value) == object_getClass(@"literal"));
        check([value retain] == value);
        [value release];
        check([value copy] == value);
        [value release];
      }
    }
    [calls release];
    puts("constant-strings=pass\nunicode=pass\nidentity=pass\nlifetime=pass");
  }
}
