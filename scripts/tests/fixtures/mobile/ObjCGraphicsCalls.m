#import "ObjCGraphicsCalls.h"

@implementation NDGraphicsCalls
- (NSUInteger)widthOfImage:(CGImageRef)image {
  return CGImageGetWidth(image);
}
- (NSUInteger)heightOfImage:(CGImageRef)image {
  return CGImageGetHeight(image);
}
- (CGImageRef)retainImage:(CGImageRef)image {
  return CGImageRetain(image);
}
- (double)alphaOfColor:(CGColorRef)color {
  return CGColorGetAlpha(color);
}
- (NSUInteger)componentsInColor:(CGColorRef)color {
  return CGColorGetNumberOfComponents(color);
}
- (NSUInteger)imageCountInSource:(CGImageSourceRef)source {
  return CGImageSourceGetCount(source);
}
@end
