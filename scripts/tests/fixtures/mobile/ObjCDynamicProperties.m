#import "ObjCDynamicProperties.h"

@implementation NDDynamicRecord
@dynamic ndEventCount, ndWeight, ndLabel;
@end

@implementation NDPropertyDriver
- (int32_t)eventCount:(NDDynamicRecord *)record {
  return record.ndEventCount;
}
- (void)setEventCount:(int32_t)value record:(NDDynamicRecord *)record {
  record.ndEventCount = value;
}
- (double)weight:(NDDynamicRecord *)record {
  return record.ndWeight;
}
- (void)setWeight:(double)value record:(NDDynamicRecord *)record {
  record.ndWeight = value;
}
- (NSString *)label:(NDDynamicRecord *)record {
  return record.ndLabel;
}
- (void)setLabel:(NSString *)value record:(NDDynamicRecord *)record {
  record.ndLabel = value;
}
@end
