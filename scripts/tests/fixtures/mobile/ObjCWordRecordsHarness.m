#import "ObjCWordRecords.h"

#import <objc/runtime.h>
#include <stdio.h>
#include <string.h>
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

static unsigned calls;
static NDWords observedSwap(id object, SEL selector, NDWords value) {
  ++calls;
  return (NDWords){value.second, value.first};
}

int main(void) {
#ifdef NEVERD_RECOVERED_ARC
  installRecovered();
#endif
  @autoreleasepool {
    NDWordRecords *object = [NDWordRecords new];
    NSArray *array = @[ @"zero", @"one", @"two", @"three" ];
    Method swap =
        class_getInstanceMethod([NDWordRecords class], @selector(swap:));
    IMP saved = method_getImplementation(swap);
    for (unsigned i = 0; i < 4096; ++i) {
      uint64_t bits = UINT64_C(0x9e3779b97f4a7c15) * i;
      NDWord word;
      memcpy(&word.value, &bits, sizeof(bits));
      NDWord w = [object word:word];
      if (w.value != word.value)
        return 1;
      NDWords value = {bits, ~bits};
      NDWords out = [object pair:value];
      if (memcmp(&out, &value, sizeof(value)))
        return 2;
      NDWords reversed = {value.second, value.first};
      out = [object swap:value];
      if (memcmp(&out, &reversed, sizeof(out)))
        return 3;
      method_setImplementation(swap, (IMP)observedSwap);
      out = [object throughCall:value];
      method_setImplementation(swap, saved);
      if (memcmp(&out, &reversed, sizeof(out)) || calls != i + 1)
        return 4;
      NDNestedWords nested = {word, ~bits};
      NDNestedWords n = [object nested:nested];
      if (memcmp(&n, &nested, sizeof(n)))
        return 5;
      NDPointerWord pointer = {&bits, ~bits};
      NDPointerWord p = [object pointer:pointer];
      if (p.pointer != &bits || p.value != ~bits)
        return 6;
      if ([object readPointer:pointer] != UINT64_MAX)
        return 7;
      NDWords expected = {bits + 3, ~bits + 7};
      out = [object three:1 b:2 c:3 pair:value tail:4];
      if (memcmp(&out, &expected, sizeof(out)))
        return 8;
      expected = (NDWords){bits + 6, ~bits + 15};
      out = [object five:1 b:2 c:3 d:4 e:5 pair:value tail:6];
      if (memcmp(&out, &expected, sizeof(out)))
        return 9;
      NSRange range = NSMakeRange(i % 4, i % 2);
      NSArray *part = [object subarray:array range:range];
      if (part.count != range.length ||
          (range.length && part[0] != array[range.location]))
        return 10;
      NSString *needle = (i % 2) ? @"cd" : @"missing";
      NSRange found = [object find:@"abcdefgh" needle:needle];
      if (found.location != ((i % 2) ? 2 : NSNotFound) ||
          found.length != ((i % 2) ? 2 : 0))
        return 11;
    }
    [object release];
    puts("word-record-checks=45056\ncall-effects=4096\nrecord-bits="
         "pass\nrecord-stack=pass");
  }
  return 0;
}
