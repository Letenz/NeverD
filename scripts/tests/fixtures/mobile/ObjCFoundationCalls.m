#import <Foundation/Foundation.h>

@interface NDFoundationCalls : NSObject
- (id)lookup:(NSDictionary *)dictionary key:(id)key;
- (NSUInteger)lengthOf:(NSString *)string;
- (id)copyObject:(id)object;
- (void)append:(id)object to:(NSMutableArray *)array;
- (double)numberValue:(NSNumber *)number;
- (void)put:(id)object
     forKey:(id<NSCopying>)key
         in:(NSMutableDictionary *)dictionary;
- (id)makeDictionary:(id __unsafe_unretained const *)objects
                keys:(id<NSCopying> __unsafe_unretained const *)keys
               count:(NSUInteger)count;
@end

@implementation NDFoundationCalls
- (id)lookup:(NSDictionary *)dictionary key:(id)key {
  return dictionary[key];
}
- (NSUInteger)lengthOf:(NSString *)string {
  return string.length;
}
- (id)copyObject:(id)object {
  return [object copy];
}
- (void)append:(id)object to:(NSMutableArray *)array {
  [array addObject:object];
}
- (double)numberValue:(NSNumber *)number {
  return number.doubleValue;
}
- (void)put:(id)object
     forKey:(id<NSCopying>)key
         in:(NSMutableDictionary *)dictionary {
  [dictionary setObject:object forKey:key];
}
- (id)makeDictionary:(id __unsafe_unretained const *)objects
                keys:(id<NSCopying> __unsafe_unretained const *)keys
               count:(NSUInteger)count {
  return [NSDictionary dictionaryWithObjects:objects forKeys:keys count:count];
}
@end
