#import "ObjCReceiverFields.h"

@implementation NDFieldOwner (Calls)
- (NSInteger)errorCode {
  return [_typedError code];
}
- (NSInteger)nestedErrorCode {
  return [_inner->_error code];
}
- (NSInteger)errorCodeAfterCall:(id)object {
  [object description];
  return [_typedError code];
}
@end

@implementation NDFieldUnrelated
- (void *)code {
  return (__bridge void *)self;
}
@end
