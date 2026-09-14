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
- (id)formatObject:(id)object number:(int)number fraction:(double)fraction;
- (id)formatPosition:(int)width fraction:(double)fraction;
- (id)formatEmpty;
- (id)formatWide:(long long)value small:(unsigned char)small;
- (void)logObject:(id)object count:(int)count fraction:(double)fraction;
- (void)logEmpty;
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
- (id)formatObject:(id)object number:(int)number fraction:(double)fraction {
  return [NSString stringWithFormat:@"%@/%d/%.2f", object, number, fraction];
}
- (id)formatPosition:(int)width fraction:(double)fraction {
  return [NSString stringWithFormat:@"%2$*1$.2f/%2$.2f", width, fraction];
}
- (id)formatEmpty {
  return [NSString stringWithFormat:@"empty-%%-\u03a9"];
}
- (id)formatWide:(long long)value small:(unsigned char)small {
  return [NSString stringWithFormat:@"%lld/%hhu", value, small];
}
- (void)logObject:(id)object count:(int)count fraction:(double)fraction {
  NSLog(@"ND_FORMAT:%@/%d/%.2f", object, count, fraction);
}
- (void)logEmpty {
  NSLog(@"ND_FORMAT:empty-%%");
}
@end
