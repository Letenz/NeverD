#import <Foundation/Foundation.h>

@interface NDPredicateFormats : NSObject
- (NSPredicate *)object:(id)value;
- (NSPredicate *)key:(NSString *)key value:(id)value;
- (NSPredicate *)minimum:(NSInteger)minimum maximum:(NSInteger)maximum;
- (NSPredicate *)count:(int)count;
- (NSPredicate *)score:(double)score;
- (NSPredicate *)quoted:(id)value;
- (NSPredicate *)always;
- (NSExpression *)expression:(long long)value;
@end
