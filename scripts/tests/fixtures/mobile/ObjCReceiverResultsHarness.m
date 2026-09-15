#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdio.h>
extern id objc_initWeak(id *, id);
extern void objc_destroyWeak(id *);
extern id objc_loadWeakRetained(id *);
@interface NDResultValue : NSObject
@property(nonatomic) double duration;
+ (double)factoryDuration:(double)value;
@end
@interface NDResultOwner : NSObject
@property(nonatomic, strong) NSError *error;
- (NSInteger)errorCode;
@end
@interface NDResultOther : NSObject
- (void *)code;
@end
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif
int main(void) {
#ifdef NEVERD_RECOVERED_ARC
  installRecovered();
#endif
  @autoreleasepool {
    NDResultOther *other = [NDResultOther new];
    for (NSInteger i = 0; i < 1024; ++i) {
      id observed = nil;
      @autoreleasepool {
        NSInteger code = i == 0   ? NSIntegerMin
                         : i == 1 ? NSIntegerMax
                                  : (i - 512) * 65537;
        double duration = (i - 512) * 0.125;
        NDResultValue *value = [NDResultValue new];
        value.duration = duration;
        NDResultOwner *owner = [NDResultOwner new];
        NSError *error = [[NSError alloc] initWithDomain:@"result"
                                                    code:code
                                                userInfo:nil];
        objc_initWeak(&observed, error);
        owner.error = error;
        [error release];
        if ([NDResultValue factoryDuration:duration] != duration ||
            value.duration != duration || [owner errorCode] != code ||
            owner.error != error || [other code] != (void *)other)
          return 1;
        [value release];
        [owner release];
      }
      if (objc_loadWeakRetained(&observed) != nil)
        return 2;
      objc_destroyWeak(&observed);
    }
    [other release];
    puts("receiver-results=5120\nlifetime-checks=1024\nidentity=pass");
  }
  return 0;
}
