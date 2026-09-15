#import <Foundation/Foundation.h>

@interface NDLoopEdges : NSObject
- (NSUInteger)fold:(NSUInteger)count
              seed:(NSUInteger)seed
            output:(NSUInteger *)output;
@end
@implementation NDLoopEdges
- (NSUInteger)fold:(NSUInteger)count
              seed:(NSUInteger)seed
            output:(NSUInteger *)output {
  NSUInteger left = count & 31;
  NSUInteger value = seed;
  *output = seed;
  while (left) {
    if (value & 1) {
      value += left * 3;
      *output = value;
      --left;
      if (left == 2)
        break;
      continue;
    }
    value = (value << 1) ^ (left + 7);
    *output = value;
    --left;
  }
  return value + left;
}
@end
