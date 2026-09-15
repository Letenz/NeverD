#import "ObjCSwiftAllocation.h"

#include <malloc/malloc.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <string.h>

extern void *NDMakeAllocationSeed(void);
extern uint64_t NDAllocationDestroyed(void);
extern void *swift_getObjectType(void *);
extern void swift_release(void *);

#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

int main(void) {
#ifdef NEVERD_RECOVERED_ARC
  installRecovered();
#endif
  @autoreleasepool {
    NDSwiftAllocation *object = [[NDSwiftAllocation alloc] init];
    void *seed = NDMakeAllocationSeed();
    void *metadata = swift_getObjectType(seed);
    const uintptr_t objectSize = class_getInstanceSize((Class)metadata);
    if (!metadata || objectSize < 16 || malloc_size(seed) < objectSize)
      return 1;
    for (uintptr_t i = 0; i < 1024; ++i) {
      const uintptr_t size = 1 + i * 7;
      const uintptr_t mask = ((uintptr_t)1 << (3 + i % 6)) - 1;
      void *bytes = [object allocateRaw:size alignment:mask];
      if (!bytes || ((uintptr_t)bytes & mask) || malloc_size(bytes) < size)
        return 2;
      memset(bytes, (int)i, size);
      for (uintptr_t j = 0; j < size; ++j)
        if (((unsigned char *)bytes)[j] != (unsigned char)i)
          return 3;
      [object freeRaw:bytes size:size alignment:mask];
      void *allocation = [object allocateObject:metadata
                                           size:objectSize
                                      alignment:7];
      if (!allocation || swift_getObjectType(allocation) != metadata ||
          malloc_size(allocation) < objectSize)
        return 4;
      [object freeUninitialized:allocation size:objectSize alignment:7];
      if (NDAllocationDestroyed() != 0)
        return 5;
    }
    swift_release(seed);
    if (NDAllocationDestroyed() != 1)
      return 6;
    [object release];
  }
  puts("swift-allocation=4096\nalignment=pass\nmemory=pass\nmetadata="
       "pass\ndestruction=pass");
  return 0;
}
