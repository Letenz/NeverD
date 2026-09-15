#import "ObjCSwiftAllocation.h"

extern void *swift_slowAlloc(uintptr_t, uintptr_t);
extern void swift_slowDealloc(void *, uintptr_t, uintptr_t);
extern void *swift_allocObject(void *, uintptr_t, uintptr_t);
extern void swift_deallocUninitializedObject(void *, uintptr_t, uintptr_t);

@implementation NDSwiftAllocation
- (void *)allocateRaw:(uintptr_t)size alignment:(uintptr_t)mask {
  return swift_slowAlloc(size, mask);
}
- (void)freeRaw:(void *)pointer size:(uintptr_t)size alignment:(uintptr_t)mask {
  swift_slowDealloc(pointer, size, mask);
}
- (void *)allocateObject:(void *)metadata
                    size:(uintptr_t)size
               alignment:(uintptr_t)mask {
  return swift_allocObject(metadata, size, mask);
}
- (void)freeUninitialized:(void *)pointer
                     size:(uintptr_t)size
                alignment:(uintptr_t)mask {
  swift_deallocUninitializedObject(pointer, size, mask);
}
@end
