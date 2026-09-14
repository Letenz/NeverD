#import <Foundation/Foundation.h>

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
@end
