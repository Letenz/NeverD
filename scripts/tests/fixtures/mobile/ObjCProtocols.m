#import <Foundation/Foundation.h>
#include <objc/runtime.h>

extern long __stack_chk_guard[8];
extern void __stack_chk_fail(void) __attribute__((noreturn));

@protocol NDSignedMetric
- (int64_t)metric;
@end
@protocol NDUnsignedMetric
- (uint64_t)metric;
@end

Protocol *NDEnumerationProtocol(void) { return @protocol(NSFastEnumeration); }
Protocol *NDMetricProtocol(void) { return @protocol(NDUnsignedMetric); }
Protocol *NDSignedMetricProtocol(void) { return @protocol(NDSignedMetric); }

@interface NDProtocolCalls : NSObject
- (NSUInteger)enumerate:(id<NSFastEnumeration>)source
                  state:(NSFastEnumerationState *)state
                objects:(id __unsafe_unretained *)objects
                  count:(NSUInteger)count;
- (uint64_t)metricOf:(id<NDSignedMetric>)source;
- (BOOL)isNegativeMetric:(id<NDSignedMetric>)source;
- (NSUInteger)countObjects:(id<NSFastEnumeration>)source;
- (NSUInteger)reportMutation:(id)object;
- (NSUInteger)checkRuntimeGuard:(uintptr_t)expected;
@end

@implementation NDProtocolCalls
- (NSUInteger)enumerate:(id<NSFastEnumeration>)source
                  state:(NSFastEnumerationState *)state
                objects:(id __unsafe_unretained *)objects
                  count:(NSUInteger)count {
  return [source countByEnumeratingWithState:state objects:objects count:count];
}
- (uint64_t)metricOf:(id<NDSignedMetric>)source {
  return (uint64_t)[source metric];
}
- (BOOL)isNegativeMetric:(id<NDSignedMetric>)source {
  return [source metric] < 0;
}
- (NSUInteger)countObjects:(id<NSFastEnumeration>)source {
  NSUInteger count = 0;
  for (id object in source)
    if (object)
      ++count;
  return count;
}
- (NSUInteger)reportMutation:(id)object {
  objc_enumerationMutation(object);
  return 73;
}
- (NSUInteger)checkRuntimeGuard:(uintptr_t)expected {
  if ((uintptr_t)__stack_chk_guard[0] != expected)
    __stack_chk_fail();
  return 73;
}
@end
