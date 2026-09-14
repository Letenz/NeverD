#import <Foundation/Foundation.h>

@interface NDStoredStrings : NSObject
- (NSArray *)arrayWithValue:(id)value;
- (NSDictionary *)dictionaryWithValue:(id)value;
- (void)writeLiteralTo:(id __unsafe_unretained *)output;
- (NSString *)literal;
@end
