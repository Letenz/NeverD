#import <Foundation/Foundation.h>
extern id objc_retain(id);
extern void objc_release(id);
extern id objc_autorelease(id);
extern id objc_retainAutorelease(id);
extern id objc_retainAutoreleasedReturnValue(id);
@interface NDAliasError : NSError
- (NSInteger)retainedCode;
- (NSInteger)autoreleasedCode;
- (NSInteger)retainAutoreleasedCode;
- (NSInteger)claimedCode;
@end
@implementation NDAliasError
- (NSInteger)retainedCode {
  id value = objc_retain(self);
  NSInteger result = [(NSError *)value code];
  objc_release(value);
  return result;
}
- (NSInteger)autoreleasedCode {
  objc_retain(self);
  id value = objc_autorelease(self);
  return [(NSError *)value code];
}
- (NSInteger)retainAutoreleasedCode {
  id value = objc_retainAutorelease(self);
  return [(NSError *)value code];
}
- (NSInteger)claimedCode {
  id value = objc_retainAutoreleasedReturnValue(self);
  NSInteger result = [(NSError *)value code];
  objc_release(value);
  return result;
}
@end
@interface NDAliasOther : NSObject
- (void *)code;
@end
@implementation NDAliasOther
- (void *)code {
  return (void *)self;
}
@end
