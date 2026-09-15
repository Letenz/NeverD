#import "ObjCEquality.h"

@implementation NDEquality
- (BOOL)equivalent:(NDEquality *)other {
  return (self.leftValue == other.leftValue ||
          [self.leftValue isEqual:other.leftValue]) &&
         (self.rightValue == other.rightValue ||
          [self.rightValue isEqual:other.rightValue]);
}
@end
