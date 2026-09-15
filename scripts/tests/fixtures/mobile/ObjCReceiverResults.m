#import <CoreSpotlight/CoreSpotlight.h>
#import <Foundation/Foundation.h>
@interface NDResultValue : NSObject
@property(nonatomic) double duration;
+ (double)factoryDuration:(double)value;
@end
@implementation NDResultValue
+ (double)factoryDuration:(double)value {
  NDResultValue *object = [self new];
  object.duration = value;
  return object.duration;
}
@end
@interface NDResultOwner : NSObject
@property(nonatomic, strong) NSError *error;
- (NSInteger)errorCode;
@end
@implementation NDResultOwner
- (NSInteger)errorCode {
  return self.error.code;
}
@end
@interface NDResultOther : NSObject
- (void *)code;
@end
@implementation NDResultOther
- (void *)code {
  return (__bridge void *)self;
}
@end
