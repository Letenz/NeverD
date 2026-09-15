#import "ObjCMetadataCalls.h"

#import <objc/runtime.h>
#include <stdio.h>
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif
extern const void *nd_metadata_oracle(uint32_t index);

int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NDMetadataCalls *object = [NDMetadataCalls new];
    for (unsigned round = 0; round < 512; ++round) {
      NDMetadataResponse values[] = {
          [object url:0],          [object date:0],
          [object characterSet:0], [object dateComponents:0],
          [object request:0],      [object indexPath:0],
          [object locale:0],       [object notification:0],
      };
      for (unsigned i = 0; i < sizeof(values) / sizeof(*values); ++i)
        if (values[i].value != nd_metadata_oracle(i) || values[i].state != 0)
          return 1;
    }
    [object release];
  }
  puts("metadata-responses=4096\nmetadata-identity=pass\ncomplete-state=pass");
  return 0;
}
