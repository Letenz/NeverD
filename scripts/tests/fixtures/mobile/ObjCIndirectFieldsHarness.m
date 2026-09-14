#import "ObjCIndirectFields.h"

#import <objc/runtime.h>
#include <stdio.h>

#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

static unsigned destroyed;
@interface NDFieldValue : NSObject
@end
@implementation NDFieldValue
- (void)dealloc {
  ++destroyed;
  [super dealloc];
}
@end

int main(void) {
#ifdef NEVERD_RECOVERED_ARC
  installRecovered();
#endif
  for (unsigned i = 0; i < 1024; ++i) {
    @autoreleasepool {
      NDIndirectFields *object = [[NDIndirectFields alloc] init];
      if ([object first] != nil || [object second] != nil)
        return 1;
      NDFieldValue *first = [[NDFieldValue alloc] init];
      NDFieldValue *second = [[NDFieldValue alloc] init];
      [object setFirst:first];
      [object setSecond:second];
      [first release];
      [second release];
      @autoreleasepool {
        if ([object first] != first || [object second] != second ||
            destroyed != 2 * i)
          return 2;
      }
      id held;
      @autoreleasepool {
        held = [[object first] retain];
        [object setFirst:second];
        if ([object first] != second || [object second] != second)
          return 3;
      }
#ifdef NEVERD_STANDALONE_SOURCE
      // Standalone method syntax cannot express .cxx_destruct yet. Exercise
      // both recovered strong stores here; the C API run also replaces the
      // actual destructor and verifies release without these explicit clears.
      [object setFirst:nil];
      [object setSecond:nil];
#endif
      [object release];
      if (destroyed != 2 * i + 1 || held != first)
        return 4;
      [held release];
      if (destroyed != 2 * i + 2)
        return 5;
    }
  }
  puts("indirect-fields=6144\nobject-identity=pass\nlifetime=pass\ndestroyed="
       "2048");
  return 0;
}
