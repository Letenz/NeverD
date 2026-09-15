#import "ObjCCRecordCalls.h"

@implementation NDCCRecords
- (NSRange)unionRange:(NSRange)a with:(NSRange)b {
  return NSUnionRange(a, b);
}
- (NSRange)intersection:(NSRange)a with:(NSRange)b {
  return NSIntersectionRange(a, b);
}
#if defined(__aarch64__) || defined(__arm64__)
- (CGFloat)width:(CGRect)rect {
  return CGRectGetWidth(rect);
}
- (CGFloat)midY:(CGRect)rect {
  return CGRectGetMidY(rect);
}
- (CGRect)standardize:(CGRect)rect {
  return CGRectStandardize(rect);
}
- (CGRect)unionRect:(CGRect)a with:(CGRect)b {
  return CGRectUnion(a, b);
}
- (CGPoint)textPosition:(CGContextRef)context {
  return CGContextGetTextPosition(context);
}
- (void)fill:(CGRect)rect context:(CGContextRef)context {
  CGContextFillRect(context, rect);
}
#endif
@end
