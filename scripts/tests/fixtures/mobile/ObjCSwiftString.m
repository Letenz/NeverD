#import <Foundation/Foundation.h>
#include <stdint.h>

extern void *nd_bridge_string(uint64_t, void *) __asm__(
    "_$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF")
    __attribute__((swiftcall));

@interface NDSwiftString : NSObject
- (NSString *)bridgeWord:(uint64_t)word storage:(void *)storage;
@end
@implementation NDSwiftString
- (NSString *)bridgeWord:(uint64_t)word storage:(void *)storage {
  return (__bridge_transfer NSString *)nd_bridge_string(word, storage);
}
@end
