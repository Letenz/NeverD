#import "ObjCReceiverFields.h"

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
    NDFieldOwner *owner = [NDFieldOwner new];
    NDFieldInner *inner = [NDFieldInner new];
    NDFieldUnrelated *other = [NDFieldUnrelated new];
    owner->_inner = inner;
    for (NSInteger i = 0; i != 1024; ++i) {
      @autoreleasepool {
        NSInteger code = i == 0   ? NSIntegerMin
                         : i == 1 ? NSIntegerMax
                                  : (i - 512) * 65537;
        NSError *error = [NSError errorWithDomain:@"fields"
                                             code:code
                                         userInfo:nil];
        owner->_typedError = i % 7 ? error : nil;
        inner->_error = i % 3 ? error : nil;
        if ([owner errorCode] != (i % 7 ? code : 0) ||
            [owner nestedErrorCode] != (i % 3 ? code : 0) ||
            [owner errorCodeAfterCall:other] != (i % 7 ? code : 0) ||
            [other code] != (void *)other)
          return 1;
      }
    }
    owner->_typedError = nil;
    inner->_error = nil;
    owner->_inner = nil;
    [owner release];
    [inner release];
    [other release];
    puts("receiver-fields=4096\nnested-types=pass\nnil-dispatch=pass");
  }
  return 0;
}
