#import <Foundation/Foundation.h>
#include <stdint.h>
extern uint64_t nd_conditional_word(uint64_t, uint64_t);
extern uint64_t nd_conditional_memory(uint64_t, const uint64_t *);
@interface NDIncomingResults : NSObject
- (uint64_t)word:(uint64_t)value flags:(uint64_t)flags;
- (uint64_t)word:(uint64_t)value memory:(const uint64_t *)memory;
@end
@implementation NDIncomingResults
- (uint64_t)word:(uint64_t)value flags:(uint64_t)flags {
  return nd_conditional_word(value, flags);
}
- (uint64_t)word:(uint64_t)value memory:(const uint64_t *)memory {
  return nd_conditional_memory(value, memory);
}
@end
