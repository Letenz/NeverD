#import "ObjCStoredStrings.h"

@implementation NDStoredStrings
- (NSArray *)arrayWithValue:(id)value {
  return @[ @"key", value, @"尾🙂" ];
}
- (NSDictionary *)dictionaryWithValue:(id)value {
  return @{@"key" : value, @"alias" : @"kept"};
}
- (void)writeLiteralTo:(id __unsafe_unretained *)output {
  *output = @"key";
}
- (NSString *)literal {
  return @"key";
}
@end
