#include <Block.h>
#import <Foundation/Foundation.h>
@interface NDBlockFactory : NSObject
- (NSUInteger (^)(void))makeCounterForArray:(NSArray *)array;
- (NSUInteger (^)(void))makeCounterForArray:(NSArray *)array
                                      other:(NSArray *)other
                                     offset:(NSUInteger)offset;
- (void *)duplicateBlock:(void *)block;
- (void)releaseBlock:(void *)block;
#if !__has_feature(objc_arc)
- (id (^)(void))holderForBlock:(NSUInteger (^)(void))block;
#endif
@end
@implementation NDBlockFactory
- (NSUInteger (^)(void))makeCounterForArray:(NSArray *)array {
  NSUInteger (^block)(void) = ^{
    return array.count;
  };
#if __has_feature(objc_arc)
  return block;
#else
  return [(id)Block_copy(block) autorelease];
#endif
}
- (NSUInteger (^)(void))makeCounterForArray:(NSArray *)array
                                      other:(NSArray *)other
                                     offset:(NSUInteger)offset {
  NSUInteger (^block)(void) = ^{
    return array.count + other.count + offset;
  };
#if __has_feature(objc_arc)
  return block;
#else
  return [(id)Block_copy(block) autorelease];
#endif
}
- (void *)duplicateBlock:(void *)block {
  return _Block_copy(block);
}
- (void)releaseBlock:(void *)block {
  _Block_release(block);
}
#if !__has_feature(objc_arc)
- (id (^)(void))holderForBlock:(NSUInteger (^)(void))block {
  id (^holder)(void) = ^id {
    return block;
  };
  return [(id)Block_copy(holder) autorelease];
}
#endif
@end
