#import <Foundation/Foundation.h>

@protocol NDValueProtocol
- (id)value;
@end
@interface NDProtocolReferences : NSObject
- (Protocol *)valueProtocol;
- (Protocol *)rootProtocol;
- (Protocol *)sameValueProtocol;
- (Protocol *)sameRootProtocol;
@end
