#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
@interface NDNativeFloating : NSObject
- (double)doubleValue:(double)x other:(double)y mode:(NSUInteger)mode output:(double *)out;
- (float)floatValue:(float)x other:(float)y mode:(NSUInteger)mode output:(float *)out;
- (NSInteger)compare:(double)x other:(double)y bias:(NSInteger)bias;
@end
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif
static void equalDouble(double a, double b) {
  uint64_t x, y; memcpy(&x, &a, sizeof(x)); memcpy(&y, &b, sizeof(y));
  if (x != y && !(isnan(a) && isnan(b))) abort();
}
static void equalFloat(float a, float b) {
  uint32_t x, y; memcpy(&x, &a, sizeof(x)); memcpy(&y, &b, sizeof(y));
  if (x != y && !(isnan(a) && isnan(b))) abort();
}
int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NDNativeFloating *driver = [NDNativeFloating new];
    const double special[] = {0.0, -0.0, INFINITY, -INFINITY, NAN, 0x1p-1022, -0x1p-1022, 0x1p-149};
    uint64_t random = UINT64_C(0xfedcba9876543210);
    for (unsigned i = 0; i < 8192; ++i) {
      random ^= random << 13; random ^= random >> 7; random ^= random << 17;
      double x = i < 128 ? special[i % 8] : (int32_t)random / 65536.0;
      double y = i < 128 ? special[(i / 8) % 8] : (int32_t)(random >> 32) / 65536.0;
      NSUInteger mode = i & 1;
      volatile double expected = mode ? x + y : x * y;
      double out = 1234;
      equalDouble([driver doubleValue:x other:y mode:mode output:&out], expected - y);
      equalDouble(out, expected);
      float xf = x, yf = y, outf = 1234;
      volatile float expectedf = mode ? xf + yf : xf * yf;
      equalFloat([driver floatValue:xf other:yf mode:mode output:&outf], expectedf - yf);
      equalFloat(outf, expectedf);
      NSInteger bias = (int32_t)random;
      if ([driver compare:x other:y bias:bias] != bias + (x < y ? 1 : 0)) abort();
    }
    [driver release];
    puts("native-floating-cases=24576\nfloating-bits=pass\nmemory-effects=pass");
  }
}
