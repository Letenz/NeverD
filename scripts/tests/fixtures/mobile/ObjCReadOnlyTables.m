#import <Foundation/Foundation.h>
#include <stdint.h>

@interface NDReadOnlyTables : NSObject
- (uint64_t)actionForKind:(uint32_t)kind;
- (int16_t)shortForKind:(uint32_t)kind;
- (uint32_t)maskedForKind:(uint32_t)kind;
- (double)doubleForKind:(uint32_t)kind;
@end
@implementation NDReadOnlyTables
- (uint64_t)actionForKind:(uint32_t)kind {
  static const uint64_t values[] = {
      UINT64_C(0x1020304050607080), UINT64_C(0xfedcba9876543210),
      UINT64_C(0x0706050403020100), UINT64_C(0x80ff007faa55cc33),
      UINT64_C(0x3141592653589793), UINT64_C(0xffff0000ffff0000),
      UINT64_C(0x8000000000000001), UINT64_C(0x13579bdf02468ace),
      UINT64_C(0xf0e0d0c0b0a09080)};
  uint32_t index = kind - 3;
  if (index > 8)
    return 2;
  return values[index];
}
- (int16_t)shortForKind:(uint32_t)kind {
  static const int16_t values[] = {-32768, 93,  -70,   8191,  -119, 2038,
                                   41,     -21, 30000, 32767, -19,  123,
                                   -512,   0,   711,   -83,   98,   -1023};
  if (kind > 17)
    return 1;
  return values[kind];
}
- (uint32_t)maskedForKind:(uint32_t)kind {
  static const uint32_t values[] = {91, 1231, 511, 73, 999, 4161, 113, 801};
  return values[kind & 7];
}
- (double)doubleForKind:(uint32_t)kind {
  static const double values[] = {0.25, -13.5, 1000.125, -0.0, 17.75};
  if (kind > 4)
    return 4.0;
  return values[kind];
}
@end
