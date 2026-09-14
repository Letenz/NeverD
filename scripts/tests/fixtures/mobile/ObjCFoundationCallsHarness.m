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
    puts("framework-iterations=2048\narray-dictionaries=17\nnil-dispatch=pass");
  }
  return 0;
}
