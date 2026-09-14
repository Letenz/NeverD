#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

@interface NDGraphicsCalls : NSObject
- (NSUInteger)widthOfImage:(CGImageRef)image;
- (NSUInteger)heightOfImage:(CGImageRef)image;
- (CGImageRef)retainImage:(CGImageRef)image;
- (double)alphaOfColor:(CGColorRef)color;
- (NSUInteger)componentsInColor:(CGColorRef)color;
- (NSUInteger)imageCountInSource:(CGImageSourceRef)source;
@end
