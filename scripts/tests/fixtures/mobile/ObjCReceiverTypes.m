#import "ObjCReceiverTypes.h"

@implementation NDIntegerReceiver
- (int32_t)sharedValue {
  return -17000001;
}
- (int32_t)readOwnValue {
  return [self sharedValue];
}
+ (double)sharedValue {
  return -13.625;
}
+ (double)readClassValue {
  return [self sharedValue];
}
@end

@implementation NDPointerReceiver
- (void *)sharedValue {
  return (__bridge void *)self;
}
- (void *)readOwnValue {
  return [self sharedValue];
}
- (void *)code {
  return (__bridge void *)self;
}
- (void *)readOwnCode {
  return [self code];
}
@end

@implementation NDTypedError
- (NSInteger)declaredCode {
  return [self code];
}
@end
