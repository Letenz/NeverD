#import <Foundation/Foundation.h>
#include <stdint.h>
extern const void *__attribute__((swiftcall))
swift_getTypeByMangledNameInContext(const char *, size_t, const void *,
                                    const void *const *);
@interface NDSwiftTypeLookup : NSObject
- (uintptr_t)lookup:(const char *)bytes length:(NSUInteger)length;
@end
@implementation NDSwiftTypeLookup
- (uintptr_t)lookup:(const char *)bytes length:(NSUInteger)length {
  return (uintptr_t)swift_getTypeByMangledNameInContext(bytes, length, 0, 0);
}
@end
