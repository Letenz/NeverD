#import "ObjCFloatingSaves.h"

#include <math.h>

@implementation NDFloatingSaves
- (double)sumSine:(double)first cosine:(double)second {
  double a = sin(first);
  double b = cos(second);
  return (a + b) + first;
}
- (double)weighted:(double)value bias:(double)bias {
  double a = sin(value + bias);
  double b = cos(value - bias);
  return (a * value) + (b * bias);
}
@end
