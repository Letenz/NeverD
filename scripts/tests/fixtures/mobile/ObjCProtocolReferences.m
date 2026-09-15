#import "ObjCProtocolReferences.h"
@implementation NDProtocolReferences
- (Protocol *)valueProtocol {
  return @protocol(NDValueProtocol);
}
- (Protocol *)rootProtocol {
  return @protocol(NSObject);
}
- (Protocol *)sameValueProtocol {
  return @protocol(NDValueProtocol);
}
- (Protocol *)sameRootProtocol {
  return @protocol(NSObject);
}
@end
