#import <Foundation/Foundation.h>

@interface NDFrameSelectors : NSObject
- (NSString *)removing:(NSString *)word from:(NSString *)input;
@end
@implementation NDFrameSelectors
- (NSString *)removing:(NSString *)word from:(NSString *)input {
  if (![word length])
    return input;
  NSArray *parts = [input componentsSeparatedByString:@":"];
  NSUInteger count = [parts count];
  NSMutableString *output = [NSMutableString stringWithCapacity:[input length]];
  if (!count)
    return input;
  [output appendString:parts[0]];
  BOOL changed = NO;
  for (NSUInteger i = 1; i < count; ++i) {
    NSString *part = parts[i];
    if ([word compare:part] == NSOrderedSame) {
      changed = YES;
      continue;
    }
    [output appendString:@":"];
    [output appendString:part];
  }
  return changed ? output : input;
}
@end
