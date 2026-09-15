#import "ObjCReceiverTypes.h"

#import <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>

#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

int main(void) {
#ifdef NEVERD_RECOVERED_ARC
  installRecovered();
#endif
  @autoreleasepool {
    NDIntegerReceiver *integer = [NDIntegerReceiver new];
    NDPointerReceiver *pointer = [NDPointerReceiver new];
    for (NSInteger i = 0; i != 1024; ++i) {
      @autoreleasepool {
        NSInteger code = i == 0   ? NSIntegerMin
                         : i == 1 ? NSIntegerMax
                                  : (i - 512) * 31337;
        NDTypedError *error = [[NDTypedError alloc] initWithDomain:@"receiver"
                                                              code:code
                                                          userInfo:nil];
        if ([integer readOwnValue] != -17000001 ||
            [NDIntegerReceiver readClassValue] != -13.625 ||
            [pointer readOwnValue] != (void *)pointer ||
            [pointer readOwnCode] != (void *)pointer ||
            [error declaredCode] != code)
          return 1;
        [error release];
      }
    }
    [pointer release];
    [integer release];
    puts("receiver-checks=5120\nclass-instance-abi=pass\nsdk-inheritance=pass");
  }
  return 0;
}
