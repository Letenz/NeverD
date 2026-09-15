#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdio.h>
extern id objc_initWeak(id *, id);
extern void objc_destroyWeak(id *);
extern id objc_loadWeakRetained(id *);
@interface NDAliasError : NSError
- (NSInteger)retainedCode;
- (NSInteger)autoreleasedCode;
- (NSInteger)retainAutoreleasedCode;
- (NSInteger)claimedCode;
@end
@interface NDAliasOther : NSObject
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
    NDAliasOther *other = [NDAliasOther new];
    for (NSInteger i = 0; i < 1024; ++i) {
      id observed = nil;
      @autoreleasepool {
        NSInteger code = i == 0   ? NSIntegerMin
                         : i == 1 ? NSIntegerMax
                                  : (i - 512) * 65537;
        NDAliasError *error = [[NDAliasError alloc] initWithDomain:@"alias"
                                                              code:code
                                                          userInfo:nil];
        objc_initWeak(&observed, error);
        if ([error retainedCode] != code || [error autoreleasedCode] != code ||
            [error retainAutoreleasedCode] != code ||
            [error claimedCode] != code || [other code] != (void *)other)
          return 1;
        [error release];
      }
      if (objc_loadWeakRetained(&observed) != nil)
        return 2;
      objc_destroyWeak(&observed);
    }
    [other release];
    puts("receiver-aliases=5120\nlifetime-checks=1024\nidentity=pass");
  }
  return 0;
}
