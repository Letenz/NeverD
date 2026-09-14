#import <Foundation/Foundation.h>
#include <stdint.h>

extern void *nd_bridge_string(uint64_t, void *) __asm__(
    "_$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF")
    __attribute__((swiftcall));
extern unsigned __int128 nd_unbridge_string(void *) __asm__(
    "_$sSS10FoundationE36_"
    "unconditionallyBridgeFromObjectiveCySSSo8NSStringCSgFZ")
    __attribute__((swiftcall));
extern void swift_bridgeObjectRelease(void *);

@interface NDSwiftString : NSObject
- (NSString *)bridgeWord:(uint64_t)word storage:(void *)storage;
- (NSString *)roundTrip:(NSString *)object;
@end
@implementation NDSwiftString
- (NSString *)bridgeWord:(uint64_t)word storage:(void *)storage {
  return (__bridge_transfer NSString *)nd_bridge_string(word, storage);
}
- (NSString *)roundTrip:(NSString *)object {
  unsigned __int128 bits = nd_unbridge_string((__bridge void *)object);
  void *storage = (void *)(uintptr_t)(bits >> 64);
  NSString *result =
      (__bridge_transfer NSString *)nd_bridge_string((uint64_t)bits, storage);
  swift_bridgeObjectRelease(storage);
  return result;
}
@end
