#import "ObjCCRecordCalls.h"

#import <objc/runtime.h>
#include <stdio.h>
#include <string.h>
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

int main(void) {
#ifdef NEVERD_RECOVERED_ARC
  installRecovered();
#endif
  @autoreleasepool {
    NDCCRecords *object = [NDCCRecords new];
#if defined(__aarch64__) || defined(__arm64__)
    unsigned char actual[32 * 32 * 4] = {0}, expected[sizeof(actual)] = {0};
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(
        actual, 32, 32, 8, 128, space, kCGImageAlphaPremultipliedLast);
    CGContextRef reference = CGBitmapContextCreate(
        expected, 32, 32, 8, 128, space, kCGImageAlphaPremultipliedLast);
    if (!context || !reference)
      return 1;
    CGContextSetRGBFillColor(context, 0.25, 0.5, 0.75, 1);
    CGContextSetRGBFillColor(reference, 0.25, 0.5, 0.75, 1);
#endif
    for (unsigned i = 0; i < 1024; ++i) {
      NSRange a = NSMakeRange((NSUInteger)i * 1000000000, i % 8 + 2);
      NSRange b = NSMakeRange(a.location + i % 11, i % 7 + 1);
      NSRange out = [object unionRange:a with:b];
      NSRange want = NSUnionRange(a, b);
      if (out.location != want.location || out.length != want.length)
        return 2;
      out = [object intersection:a with:b];
      want = NSIntersectionRange(a, b);
      if (out.location != want.location || out.length != want.length)
        return 3;
#if defined(__aarch64__) || defined(__arm64__)
      CGRect r = CGRectMake((int)i - 512.5, i / 4.0, (int)(i % 9) - 4,
                            7 - (int)(i % 17));
      if ([object width:r] != CGRectGetWidth(r))
        return 4;
      if ([object midY:r] != CGRectGetMidY(r))
        return 5;
      CGRect result = [object standardize:r], required = CGRectStandardize(r);
      if (memcmp(&result, &required, sizeof(result)))
        return 6;
      CGRect other = CGRectMake(-1, 2, i % 11, i % 13);
      result = [object unionRect:r with:other];
      required = CGRectUnion(r, other);
      if (memcmp(&result, &required, sizeof(result)))
        return 7;
      CGContextSetTextPosition(context, i / 8.0, -(double)i / 4.0);
      CGPoint position = [object textPosition:context];
      if (position.x != i / 8.0 || position.y != -(double)i / 4.0)
        return 8;
      CGContextClearRect(context, CGRectMake(0, 0, 32, 32));
      CGContextClearRect(reference, CGRectMake(0, 0, 32, 32));
      r = CGRectMake(i % 15, (i / 3) % 16, i % 17, i % 19);
      [object fill:r context:context];
      CGContextFillRect(reference, r);
      if (memcmp(actual, expected, sizeof(actual)))
        return 9;
#endif
    }
#if defined(__aarch64__) || defined(__arm64__)
    CGContextRelease(context);
    CGContextRelease(reference);
    CGColorSpaceRelease(space);
    puts("c-record-checks=8192\nbitmap-effects=1024");
#else
    puts("c-record-checks=2048");
#endif
    [object release];
  }
  return 0;
}
