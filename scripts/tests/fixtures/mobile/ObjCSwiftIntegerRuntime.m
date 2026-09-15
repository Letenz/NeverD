#import <Foundation/Foundation.h>
#include <stdint.h>
extern void *swift_unknownObjectRetain_n(void *, uint32_t);
extern void swift_unknownObjectRelease(void *);
extern void *swift_getObjCClassMetadata(void *);
extern _Bool swift_dynamicCast(void *, void *, void *, void *, uintptr_t);
@interface NDSwiftIntegerRuntime : NSObject
- (void *)retainObject:(id)object times:(uint32_t)count;
- (uint8_t)object:(id)object canCastToClass:(Class)target;
@end
@implementation NDSwiftIntegerRuntime
- (void *)retainObject:(id)object times:(uint32_t)count {
  return swift_unknownObjectRetain_n((__bridge void *)object, count);
}
- (uint8_t)object:(id)object canCastToClass:(Class)target {
  void *source = (__bridge void *)object;
  void *destination = 0;
  _Bool result = swift_dynamicCast(
      &destination, &source,
      swift_getObjCClassMetadata((__bridge void *)[NSObject class]),
      swift_getObjCClassMetadata((__bridge void *)target), 0);
  if (result)
    swift_unknownObjectRelease(destination);
  return result;
}
@end
