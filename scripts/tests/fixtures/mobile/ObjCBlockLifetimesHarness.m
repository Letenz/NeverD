#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
@interface NDBlockFactory : NSObject
- (NSUInteger (^)(void))makeCounterForArray:(NSArray *)array;
- (NSUInteger (^)(void))makeCounterForArray:(NSArray *)array
                                      other:(NSArray *)other
                                     offset:(NSUInteger)offset;
@end
static unsigned destroyed;
@interface NDLifetimeToken : NSObject
@end
@implementation NDLifetimeToken
- (void)dealloc {
  ++destroyed;
  [super dealloc];
}
@end
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif
__attribute__((noinline)) static unsigned overwriteStack(unsigned seed) {
  volatile unsigned char bytes[16384];
  for (unsigned i = 0; i < sizeof(bytes); ++i)
    bytes[i] = (i + seed) * 37;
  return bytes[seed % sizeof(bytes)];
}
int main(void) {
#ifdef NEVERD_RECOVERED_ARC
  installRecovered();
#endif
  for (unsigned i = 0; i < 1024; ++i) {
    NSUInteger (^saved)(void);
    NSUInteger (^combined)(void);
    NSMutableArray *observed;
    @autoreleasepool {
      NDBlockFactory *factory = [NDBlockFactory new];
      NDLifetimeToken *token = [NDLifetimeToken new];
      observed = [[NSMutableArray alloc] initWithObjects:token, nil];
      saved = [[factory makeCounterForArray:observed] copy];
      combined = [[factory makeCounterForArray:observed other:observed
                                        offset:i] copy];
      [token release];
      [observed release];
      [factory release];
    }
    (void)overwriteStack(i);
    if (destroyed != i || saved() != 1)
      return 1;
    NSUInteger (^duplicate)(void) = [saved copy];
    [saved release];
    if (destroyed != i || duplicate() != 1)
      return 2;
    @autoreleasepool {
      [observed addObject:@"retained"];
    }
    if (duplicate() != 2 || combined() != 4 + i)
      return 3;
    [duplicate release];
    if (destroyed != i || combined() != 4 + i)
      return 4;
    [combined release];
    if (destroyed != i + 1)
      return 5;
  }
  puts("escaping-blocks=1024\ncopy-dispose=pass\nmutated-captures=pass");
  return 0;
}
