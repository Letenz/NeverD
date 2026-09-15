#import <Foundation/Foundation.h>

@interface NDIntegerReceiver : NSObject
- (int32_t)sharedValue;
- (int32_t)readOwnValue;
+ (double)sharedValue;
+ (double)readClassValue;
@end

@interface NDPointerReceiver : NSObject
- (void *)sharedValue;
- (void *)readOwnValue;
- (void *)code;
- (void *)readOwnCode;
@end

@interface NDTypedError : NSError
- (NSInteger)declaredCode;
@end
