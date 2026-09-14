#import <Foundation/Foundation.h>
@interface NDBlockFactory : NSObject
- (NSUInteger (^)(void))makeCounterForArray:(NSArray *)array;
- (NSUInteger (^)(void))makeCounterForArray:(NSArray *)array
                                      other:(NSArray *)other
                                     offset:(NSUInteger)offset;
@end
@implementation NDBlockFactory
- (NSUInteger (^)(void))makeCounterForArray:(NSArray *)array {
  return ^{
    return array.count;
  };
}
- (NSUInteger (^)(void))makeCounterForArray:(NSArray *)array
                                      other:(NSArray *)other
                                     offset:(NSUInteger)offset {
  return ^{
    return array.count + other.count + offset;
  };
}
@end
