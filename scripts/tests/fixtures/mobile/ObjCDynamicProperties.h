#import <CoreData/CoreData.h>

@protocol NDDynamicPropertyEvidence
@optional
@property(nonatomic, readonly) NSInteger ndOptionalValue;
@end

@interface NDDynamicRecord : NSManagedObject <NDDynamicPropertyEvidence>
@property(nonatomic) int32_t ndEventCount;
@property(nonatomic) double ndWeight;
@property(nonatomic, copy) NSString *ndLabel;
@end

@interface NDPropertyDriver : NSObject
- (int32_t)eventCount:(NDDynamicRecord *)record;
- (void)setEventCount:(int32_t)value record:(NDDynamicRecord *)record;
- (double)weight:(NDDynamicRecord *)record;
- (void)setWeight:(double)value record:(NDDynamicRecord *)record;
- (NSString *)label:(NDDynamicRecord *)record;
- (void)setLabel:(NSString *)value record:(NDDynamicRecord *)record;
@end
