#import "ObjCStoredStrings.h"

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
    NDStoredStrings *object = [[NDStoredStrings alloc] init];
    NSObject *value = [[NSObject alloc] init];
    for (unsigned i = 0; i < 1024; ++i) {
      NSArray *array;
      NSDictionary *dictionary;
      id stored = nil;
      @autoreleasepool {
        array = [[object arrayWithValue:value] retain];
        dictionary = [[object dictionaryWithValue:value] retain];
        [object writeLiteralTo:&stored];
      }
      NSString *literal = [object literal];
      if ([array count] != 3 || [array objectAtIndex:0] != literal ||
          [array objectAtIndex:1] != value ||
          ![[array objectAtIndex:2] isEqualToString:@"尾🙂"] ||
          [dictionary count] != 2 ||
          [dictionary objectForKey:literal] != value ||
          ![[dictionary objectForKey:@"alias"] isEqualToString:@"kept"] ||
          stored != literal || ![stored isEqualToString:@"key"])
        return 1;
      [dictionary release];
      [array release];
    }
    [value release];
    [object release];
    puts("stored-strings=4096\nobject-identity=pass\nlifetime=pass");
  }
  return 0;
}
