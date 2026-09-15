#import "ObjCFloatingSaves.h"

#include <math.h>
#import <objc/runtime.h>
#include <stdio.h>

#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

int main(void) {
#ifdef NEVERD_RECOVERED_ARC
  installRecovered();
#endif
  @autoreleasepool {
    NDFloatingSaves *driver = [NDFloatingSaves new];
    for (int i = -1024; i < 1024; ++i) {
      double x = i / 128.0;
      double y = (i % 31) / 32.0;
      double sum = [driver sumSine:x cosine:y];
      double weighted = [driver weighted:x bias:y];
      if (!isfinite(sum) || !isfinite(weighted) ||
          fabs(sum - ((sin(x) + cos(y)) + x)) > 1e-12 ||
          fabs(weighted - (sin(x + y) * x + cos(x - y) * y)) > 1e-12)
        return 1;
    }
    puts("floating-saves=4096\nvalues-across-calls=pass");
    [driver release];
  }
  return 0;
}
