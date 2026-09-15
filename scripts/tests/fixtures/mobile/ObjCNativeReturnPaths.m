#import <Foundation/Foundation.h>
#include <stdint.h>
extern uint64_t NDPathResult(uint64_t, unsigned, uint64_t *);
@interface NDNativeReturnPaths : NSObject
- (uint64_t)adjusted:(uint64_t)value
              choose:(unsigned)choice
              output:(uint64_t *)output;
@end
@implementation NDNativeReturnPaths
- (uint64_t)adjusted:(uint64_t)value
              choose:(unsigned)choice
              output:(uint64_t *)output {
  return NDPathResult(value, choice, output);
}
@end
