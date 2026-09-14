#import "ObjCSystemData.h"

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
    NDSystemData *object = [[NDSystemData alloc] init];
    for (unsigned i = 0; i < 1024; ++i) {
      NSArray *array;
      NSDictionary *dictionary;
      NSNumber *yes;
      NSNumber *no;
      @autoreleasepool {
        array = [[object emptyArray] retain];
        dictionary = [[object emptyDictionary] retain];
        yes = [[object trueObject] retain];
        no = [[object falseObject] retain];
      }
      if (array != [object emptyArrayAlias] || array != @[] ||
          [array count] != 0 || dictionary != @{} || [dictionary count] != 0 ||
          yes != @YES || no != @NO || ![yes boolValue] || [no boolValue] ||
          [object contextSaveName] !=
              NSManagedObjectContextDidSaveNotification ||
          [object gifDictionaryKey] !=
              (NSString *)kCGImagePropertyGIFDictionary ||
          [object searchableItemIdentifier] !=
              CSSearchableItemActivityIdentifier ||
          [object colorSpaceName] != (NSString *)kCGColorSpaceSRGB)
        return 1;
      [array release];
      [dictionary release];
      [yes release];
      [no release];
    }
    [object release];
    puts("system-data=9216\nsingletons=pass\nframework-identity=pass\nlifetime="
         "pass");
  }
  return 0;
}
