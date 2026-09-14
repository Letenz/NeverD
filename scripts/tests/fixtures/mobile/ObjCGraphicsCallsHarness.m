#import "ObjCGraphicsCalls.h"

#import <objc/runtime.h>
#include <stdio.h>

#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

int main(void) {
#ifdef NEVERD_RECOVERED_ARC
  installRecovered();
#endif
  @autoreleasepool {
    unsigned char pixels[7 * 3 * 4] = {0};
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(
        pixels, 7, 3, 8, 7 * 4, space, kCGImageAlphaPremultipliedLast);
    if (!context)
      return 1;
    CGImageRef image = CGBitmapContextCreateImage(context);
    CFMutableDataRef data = CFDataCreateMutable(NULL, 0);
    CGImageDestinationRef destination =
        CGImageDestinationCreateWithData(data, CFSTR("public.png"), 1, NULL);
    if (!image || !destination)
      return 2;
    CGImageDestinationAddImage(destination, image, NULL);
    if (!CGImageDestinationFinalize(destination))
      return 3;
    CGImageSourceRef source = CGImageSourceCreateWithData(data, NULL);
    const CGFloat components[] = {0.125, 0.5, 0.75, 0.25};
    CGColorRef color = CGColorCreate(space, components);
    if (!source || !color)
      return 4;
    NDGraphicsCalls *driver = [NDGraphicsCalls new];
    for (unsigned i = 0; i < 1024; ++i) {
      if ([driver widthOfImage:image] != 7 ||
          [driver heightOfImage:image] != 3 ||
          [driver alphaOfColor:color] != 0.25 ||
          [driver componentsInColor:color] != 4 ||
          [driver imageCountInSource:source] != 1)
        return 5;
      CGImageRef retained = [driver retainImage:image];
      if (retained != image)
        return 6;
      CGImageRelease(retained);
    }
    puts("graphics-queries=6144\nimage-identity=pass\npng-count=1");
    [driver release];
    CGColorRelease(color);
    CFRelease(source);
    CFRelease(destination);
    CFRelease(data);
    CGImageRelease(image);
    CGContextRelease(context);
    CGColorSpaceRelease(space);
  }
  return 0;
}
