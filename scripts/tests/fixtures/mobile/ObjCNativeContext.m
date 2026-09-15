#import <Foundation/Foundation.h>
#include <stdint.h>
extern uint64_t nd_context_bridge(uint64_t, const uint64_t *);
extern uint64_t nd_context_only_bridge(const uint64_t *);
@interface NDNativeContext : NSObject
- (uint64_t)word:(uint64_t)value context:(const uint64_t *)context;
- (uint64_t)contextWord:(const uint64_t *)context;
@end
@implementation NDNativeContext
- (uint64_t)word:(uint64_t)value context:(const uint64_t *)context {
  return nd_context_bridge(value, context);
}
- (uint64_t)contextWord:(const uint64_t *)context {
  return nd_context_only_bridge(context);
}
@end
