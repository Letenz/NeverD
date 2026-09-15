#import "ObjCWordRecords.h"

@implementation NDWordRecords
- (NDWord)word:(NDWord)value {
  return value;
}
- (NDWords)pair:(NDWords)value {
  return value;
}
- (NDWords)swap:(NDWords)value {
  return (NDWords){value.second, value.first};
}
- (NDWords)throughCall:(NDWords)value {
  return [self swap:value];
}
- (NDNestedWords)nested:(NDNestedWords)value {
  return value;
}
- (NDPointerWord)pointer:(NDPointerWord)value {
  return value;
}
- (uint64_t)readPointer:(NDPointerWord)value {
  return *value.pointer + value.value;
}
- (NDWords)three:(uint64_t)a
               b:(uint64_t)b
               c:(uint64_t)c
            pair:(NDWords)value
            tail:(uint64_t)tail {
  return (NDWords){value.first + a + b, value.second + c + tail};
}
- (NDWords)five:(uint64_t)a
              b:(uint64_t)b
              c:(uint64_t)c
              d:(uint64_t)d
              e:(uint64_t)e
           pair:(NDWords)value
           tail:(uint64_t)tail {
  return (NDWords){value.first + a + b + c, value.second + d + e + tail};
}
- (NSArray *)subarray:(NSArray *)array range:(NSRange)range {
  return [array subarrayWithRange:range];
}
- (NSRange)find:(NSString *)text needle:(NSString *)needle {
  return [text rangeOfString:needle];
}
@end
