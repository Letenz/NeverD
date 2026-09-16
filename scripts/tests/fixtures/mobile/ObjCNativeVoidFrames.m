#import <Foundation/Foundation.h>
#include <stdint.h>
extern void swift_unknownObjectRelease(void *);
static __attribute__((noinline)) void releasePair(void *first, void *second,
                                                uintptr_t active) {
  if (active) {
    swift_unknownObjectRelease(first);
    swift_unknownObjectRelease(second);
  }
}
static __attribute__((noinline, disable_tail_calls)) void
releasePairNoTail(void *first, void *second, uintptr_t active) {
  if (active) {
    swift_unknownObjectRelease(first);
    swift_unknownObjectRelease(second);
  }
}
// Deliberately reads an unspecified machine result. This method must remain
// unrecovered and is never called by the runtime harness.
extern uintptr_t unprovenPairResult(void *, void *, uintptr_t)
    __asm__("_releasePair");
@interface NDVoidFrames : NSObject
- (void)releaseFirst:(void *)first second:(void *)second active:(NSUInteger)active;
- (NSUInteger)releaseFirst:(void *)first second:(void *)second
                   active:(NSUInteger)active result:(NSUInteger)value;
- (NSUInteger)unprovenResult:(void *)first second:(void *)second
                     active:(NSUInteger)active;
@end
@implementation NDVoidFrames
- (void)releaseFirst:(void *)first second:(void *)second active:(NSUInteger)active {
  releasePair(first, second, active);
}
- (NSUInteger)releaseFirst:(void *)first second:(void *)second
                   active:(NSUInteger)active result:(NSUInteger)value {
  releasePairNoTail(first, second, active);
  return value;
}
- (NSUInteger)unprovenResult:(void *)first second:(void *)second
                     active:(NSUInteger)active {
  return unprovenPairResult(first, second, active);
}
@end
