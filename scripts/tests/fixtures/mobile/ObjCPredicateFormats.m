#import "ObjCPredicateFormats.h"

@implementation NDPredicateFormats
- (NSPredicate *)object:(id)value {
  return [NSPredicate predicateWithFormat:@"SELF == %@", value];
}
- (NSPredicate *)key:(NSString *)key value:(id)value {
  return [NSPredicate predicateWithFormat:@"%K == %@", key, value];
}
- (NSPredicate *)minimum:(NSInteger)minimum maximum:(NSInteger)maximum {
  return [NSPredicate predicateWithFormat:@"SELF >= %ld AND SELF < %ld",
                                          (long)minimum, (long)maximum];
}
- (NSPredicate *)count:(int)count {
  return [NSPredicate predicateWithFormat:@"SELF == %d", count];
}
- (NSPredicate *)score:(double)score {
  return [NSPredicate predicateWithFormat:@"SELF > %f", score];
}
- (NSPredicate *)quoted:(id)value {
  return [NSPredicate predicateWithFormat:@"SELF == '%@' OR SELF == %@", value];
}
- (NSPredicate *)always {
  return [NSPredicate predicateWithFormat:@"TRUEPREDICATE"];
}
- (NSExpression *)expression:(long long)value {
  return [NSExpression expressionWithFormat:@"%lld + 7", value];
}
@end
