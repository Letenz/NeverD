#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

@interface NDCCRecords : NSObject
- (NSRange)unionRange:(NSRange)a with:(NSRange)b;
- (NSRange)intersection:(NSRange)a with:(NSRange)b;
#if defined(__aarch64__) || defined(__arm64__)
- (CGFloat)width:(CGRect)rect;
- (CGFloat)midY:(CGRect)rect;
- (CGRect)standardize:(CGRect)rect;
- (CGRect)unionRect:(CGRect)a with:(CGRect)b;
- (CGPoint)textPosition:(CGContextRef)context;
- (void)fill:(CGRect)rect context:(CGContextRef)context;
#endif
@end
