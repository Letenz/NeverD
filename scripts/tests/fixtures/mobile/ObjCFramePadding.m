#import <Foundation/Foundation.h>
@interface NDFramePadding : NSObject
@property(nonatomic) BOOL horizontal;
@property(nonatomic) BOOL vertical;
- (NSString *)key;
@end
@implementation NDFramePadding
- (NSString *)key {
  return
      [NSString stringWithFormat:@"flip-%d-%d", self.horizontal, self.vertical];
}
@end
