#import <Foundation/Foundation.h>
#include <stdint.h>
extern void swift_unknownObjectRelease(void *);
static __attribute__((noinline)) void releaseIfActive(void *object, uintptr_t active) {
  if (active) swift_unknownObjectRelease(object);
}
// This declaration intentionally observes an unspecified machine result.
// Its method must stay unrecovered; it is never executed by the harness.
extern uintptr_t unprovenReleaseResult(void *, uintptr_t) __asm__("_releaseIfActive");
@interface NDVoidForwarders : NSObject
- (NSUInteger)unprovenResult:(void *)object active:(NSUInteger)active;
- (void)releaseObject:(void *)object active:(NSUInteger)active;
- (NSUInteger)releaseObject:(void *)object active:(NSUInteger)active result:(NSUInteger)value;
@end
@implementation NDVoidForwarders
- (NSUInteger)unprovenResult:(void *)object active:(NSUInteger)active { return unprovenReleaseResult(object, active); }
- (void)releaseObject:(void *)object active:(NSUInteger)active { releaseIfActive(object, active); }
- (NSUInteger)releaseObject:(void *)object active:(NSUInteger)active result:(NSUInteger)value { releaseIfActive(object, active); return value; }
@end
