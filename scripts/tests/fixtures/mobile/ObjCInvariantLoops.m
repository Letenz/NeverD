#import "ObjCInvariantLoops.h"

@implementation NDInvariantLoops
- (NSUInteger)nestedNumbers:(NSArray *)groups {
  NSUInteger total = 0;
  for (NSArray *values in groups)
    for (NSNumber *value in values)
      if ([value isKindOfClass:[NSNumber class]])
        total += value.unsignedIntegerValue;
  return total;
}
- (NSUInteger)nestedStrings:(NSArray *)groups {
  NSUInteger total = 0;
  for (NSArray *values in groups)
    for (NSString *value in values)
      if ([value isKindOfClass:[NSString class]])
        total += value.length;
  return total;
}
@end
