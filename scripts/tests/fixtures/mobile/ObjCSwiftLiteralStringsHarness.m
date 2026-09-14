#import "ObjCSwiftLiteralStrings.h"

#import <objc/runtime.h>
#include <stdio.h>
#include <string.h>

@interface NDSwiftLiteralStrings (LiteralMethods)
- (NSString *)asciiLiteral;
- (NSString *)sameAsciiLiteral;
- (NSString *)unicodeLiteral;
@end

#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

int main(void) {
#ifdef NEVERD_RECOVERED_ARC
  installRecovered();
#endif
  @autoreleasepool {
    NDSwiftLiteralStrings *driver = [NDSwiftLiteralStrings new];
    const char *ascii =
        "A compiler-owned immutable string with more than fifteen bytes";
    const char *unicode = "不可变字符串🙂 café e\xcc\x81";
    unsigned identical = 0;
    for (unsigned i = 0; i < 1024; ++i) {
      NSString *first, *second, *third;
      @autoreleasepool {
        first = [[driver asciiLiteral] retain];
        second = [[driver sameAsciiLiteral] retain];
        third = [[driver unicodeLiteral] retain];
      }
      if (strcmp(first.UTF8String, ascii) || strcmp(second.UTF8String, ascii) ||
          strcmp(third.UTF8String, unicode) ||
          ![first isEqualToString:second] ||
          object_getClass(first) != object_getClass(second) ||
          object_getClass(first) != object_getClass(third))
        return 1;
      identical += first == second;
      [first release];
      [second release];
      [third release];
    }
    printf(
        "swift-literals=3072\nutf8=pass\nlifetime=pass\nidentical-objects=%u\n",
        identical);
    [driver release];
  }
  return 0;
}
