#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Metadata identities are actual Swift standard-library exports, not copies.
extern const unsigned char NDIntMetadata[] __asm__("_$sSiN");
extern const unsigned char NDUIntMetadata[] __asm__("_$sSuN");
extern const unsigned char NDStringMetadata[] __asm__("_$sSSN");
extern const unsigned char NDBoolMetadata[] __asm__("_$sSbN");
extern const unsigned char NDDoubleMetadata[] __asm__("_$sSdN");
@interface NDSwiftTypeLookup : NSObject
- (uintptr_t)lookup:(const char *)bytes length:(NSUInteger)length;
@end
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif
int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NDSwiftTypeLookup *driver = [NDSwiftTypeLookup new];
    const char *names[] = {"Si", "Su", "SS", "Sb", "Sd"};
    const void *metadata[] = {NDIntMetadata, NDUIntMetadata, NDStringMetadata,
                              NDBoolMetadata, NDDoubleMetadata};
    unsigned cases = 0;
    for (unsigned i = 0; i < 4096; ++i) {
      for (unsigned t = 0; t < 5; ++t) {
        char buffer[32];
        memset(buffer, '!', sizeof(buffer));
        const unsigned offset = i % 16;
        memcpy(buffer + offset, names[t], 2);
        const uintptr_t result = [driver lookup:buffer + offset length:2];
        if (result != (uintptr_t)metadata[t]) {
          fprintf(stderr, "Swift metadata mismatch at input %u type %u\n", i,
                  t);
          abort();
        }
        ++cases;
      }
    }
    [driver release];
    if (cases != 20480)
      abort();
    puts("swift-type-lookup-cases=20480\nmetadata-identity=pass\nbyte-length="
         "pass");
  }
}
