#import <Foundation/Foundation.h>
#include <stdint.h>
extern uint64_t NDPathResult(uint64_t, unsigned, uint64_t *);
extern uint64_t NDWideLeaf(uint64_t);
@interface NDNativeReturnPaths : NSObject
- (uint64_t)adjusted:(uint64_t)value
              choose:(unsigned)choice
              output:(uint64_t *)output;
- (uint64_t)wideLeaf:(uint64_t)value;
@end
@implementation NDNativeReturnPaths
- (uint64_t)wideLeaf:(uint64_t)value {
  return NDWideLeaf(value);
}
- (uint64_t)adjusted:(uint64_t)value
              choose:(unsigned)choice
              output:(uint64_t *)output {
  return NDPathResult(value, choice, output);
}
@end
