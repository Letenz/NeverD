#import <Foundation/Foundation.h>

@interface NDFieldInner : NSObject {
@public
  __unsafe_unretained NSError *_error;
}
@end

@interface NDFieldOwner : NSObject {
@public
  __unsafe_unretained NSError *_typedError;
  __unsafe_unretained NDFieldInner *_inner;
}
@end

@interface NDFieldOwner (Calls)
- (NSInteger)errorCode;
- (NSInteger)nestedErrorCode;
- (NSInteger)errorCodeAfterCall:(id)object;
@end

@interface NDFieldUnrelated : NSObject
- (void *)code;
@end
