#import <Foundation/Foundation.h>
static __attribute__((noinline)) double blendDouble(double x, double y, NSUInteger mode, double *out) {
  double value = mode & 1 ? x + y : x * y;
  *out = value;
  return value - y;
}
static __attribute__((noinline)) float blendFloat(float x, float y, NSUInteger mode, float *out) {
  float value = mode & 1 ? x + y : x * y;
  *out = value;
  return value - y;
}
static __attribute__((noinline)) NSInteger compareDouble(double x, double y, NSInteger bias) {
  return bias + (x < y ? 1 : 0);
}
@interface NDNativeFloating : NSObject
- (double)doubleValue:(double)x other:(double)y mode:(NSUInteger)mode output:(double *)out;
- (float)floatValue:(float)x other:(float)y mode:(NSUInteger)mode output:(float *)out;
- (NSInteger)compare:(double)x other:(double)y bias:(NSInteger)bias;
@end
@implementation NDNativeFloating
- (double)doubleValue:(double)x other:(double)y mode:(NSUInteger)mode output:(double *)out { return blendDouble(x, y, mode, out); }
- (float)floatValue:(float)x other:(float)y mode:(NSUInteger)mode output:(float *)out { return blendFloat(x, y, mode, out); }
- (NSInteger)compare:(double)x other:(double)y bias:(NSInteger)bias { return compareDouble(x, y, bias); }
@end
