#import <Foundation/Foundation.h>

typedef struct NDWord {
  int64_t value;
} NDWord;
typedef struct NDWords {
  uint64_t first, second;
} NDWords;
typedef struct NDNestedWords {
  NDWord first;
  uint64_t second;
} NDNestedWords;
typedef struct NDPointerWord {
  const uint64_t *pointer;
  uint64_t value;
} NDPointerWord;

@interface NDWordRecords : NSObject
- (NDWord)word:(NDWord)value;
- (NDWords)pair:(NDWords)value;
- (NDWords)swap:(NDWords)value;
- (NDWords)throughCall:(NDWords)value;
- (NDNestedWords)nested:(NDNestedWords)value;
- (NDPointerWord)pointer:(NDPointerWord)value;
- (uint64_t)readPointer:(NDPointerWord)value;
- (NDWords)three:(uint64_t)a
               b:(uint64_t)b
               c:(uint64_t)c
            pair:(NDWords)value
            tail:(uint64_t)tail;
- (NDWords)five:(uint64_t)a
              b:(uint64_t)b
              c:(uint64_t)c
              d:(uint64_t)d
              e:(uint64_t)e
           pair:(NDWords)value
           tail:(uint64_t)tail;
- (NSArray *)subarray:(NSArray *)array range:(NSRange)range;
- (NSRange)find:(NSString *)text needle:(NSString *)needle;
@end
