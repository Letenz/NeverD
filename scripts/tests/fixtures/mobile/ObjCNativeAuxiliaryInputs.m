#import <Foundation/Foundation.h>
#include <stdint.h>
extern uint64_t nd_auxiliary_fill_bridge(uint64_t, uint64_t *);
extern uint64_t nd_auxiliary_add_bridge(uint64_t, uint64_t *);
@interface NDNativeAuxiliaryInputs : NSObject
- (uint64_t)fillWithWord:(uint64_t)value buffer:(uint64_t *)buffer;
- (uint64_t)addWord:(uint64_t)value buffer:(uint64_t *)buffer;
@end
@implementation NDNativeAuxiliaryInputs
- (uint64_t)fillWithWord:(uint64_t)value buffer:(uint64_t *)buffer {
  return nd_auxiliary_fill_bridge(value, buffer);
}
- (uint64_t)addWord:(uint64_t)value buffer:(uint64_t *)buffer {
  return nd_auxiliary_add_bridge(value, buffer);
}
@end
