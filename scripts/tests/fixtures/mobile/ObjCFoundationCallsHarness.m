#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NDFoundationCalls *calls = [NDFoundationCalls new];
    NSMutableDictionary *dictionary = [NSMutableDictionary dictionary];
    NSMutableArray *array = [NSMutableArray array];
    NSMutableString *string = [NSMutableString string];
    NSUInteger characters = 0;
    uint64_t bits = UINT64_C(0x8000000000000000);
    for (unsigned i = 0; i < 2048; ++i) {
      NSString *key = [NSString stringWithFormat:@"key-%u", i];
      NSNumber *object = @(i);
      [calls put:object forKey:key in:dictionary];
      if ([calls lookup:dictionary key:key] != object)
        return 1;
      [calls append:object to:array];
      if (array.count != i + 1 || array.lastObject != object)
        return 2;
      id copy = [calls copyObject:string];
      [string appendString:@"A\U0001F30D\u03a9"];
      if ([copy length] != characters || [copy isEqualToString:string])
        return 3;
      characters += 4;
      if ([calls lengthOf:string] != characters)
        return 4;
      double value;
      memcpy(&value, &bits, sizeof(value));
      NSNumber *number = [NSNumber numberWithDouble:value];
      double expected = number.doubleValue;
      double actual = [calls numberValue:number];
      if (memcmp(&expected, &actual, sizeof(actual)))
        return 5;
      bits = bits * UINT64_C(6364136223846793005) + 1;
    }
    if ([calls lookup:dictionary key:@"absent"] || [calls lengthOf:nil] ||
        [calls copyObject:nil])
      return 6;
    id __unsafe_unretained objects[16];
    id<NSCopying> __unsafe_unretained keys[16];
    for (unsigned i = 0; i < 16; ++i) {
      objects[i] = array[i];
      keys[i] = array[i];
    }
    for (unsigned count = 0; count <= 16; ++count) {
      NSDictionary *result = [calls makeDictionary:objects
                                              keys:keys
                                             count:count];
      if (result.count != count)
        return 7;
      for (unsigned i = 0; i < count; ++i)
        if (result[keys[i]] != objects[i])
          return 8;
    }
    for (int i = -32; i <= 32; ++i) {
      for (int j = -8; j <= 8; ++j) {
        id object = i & 1 ? @"object" : nil;
        double fraction = j / 4.0;
        NSString *expected =
            [NSString stringWithFormat:@"%@/%d/%.2f", object, i, fraction];
        if (![[calls formatObject:object number:i
                         fraction:fraction] isEqualToString:expected])
          return 9;
        expected = [NSString stringWithFormat:@"%2$*1$.2f/%2$.2f", i, fraction];
        if (![[calls formatPosition:i
                           fraction:fraction] isEqualToString:expected])
          return 10;
      }
      long long wide = 0x123456789abcll + i;
      unsigned char small = (unsigned char)i;
      NSString *expected =
          [NSString stringWithFormat:@"%lld/%hhu", wide, small];
      if (![[calls formatWide:wide small:small] isEqualToString:expected])
        return 11;
    }
    if (![[calls formatEmpty] isEqualToString:@"empty-%-\u03a9"])
      return 12;
    for (int i = 0; i < 3; ++i)
      [calls logObject:i & 1 ? @"object" : nil
                 count:i * 129 - 1
              fraction:i / 4.0];
    [calls logEmpty];
    puts("variadic-formats=2276");
    puts("framework-iterations=2048\narray-dictionaries=17\nnil-dispatch=pass");
  }
  return 0;
}
