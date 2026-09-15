#import <Foundation/Foundation.h>
#include <stdint.h>

@interface NDSwiftAllocation : NSObject
- (void *)allocateRaw:(uintptr_t)size alignment:(uintptr_t)mask;
- (void)freeRaw:(void *)pointer size:(uintptr_t)size alignment:(uintptr_t)mask;
- (void *)allocateObject:(void *)metadata
                    size:(uintptr_t)size
               alignment:(uintptr_t)mask;
- (void)freeUninitialized:(void *)pointer
                     size:(uintptr_t)size
                alignment:(uintptr_t)mask;
@end
