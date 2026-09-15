#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
@interface NDSavedValues : NSObject
@property(nonatomic) BOOL flag;
@property(nonatomic) unsigned char byte;
@property(nonatomic) unsigned short word;
@property(nonatomic) int integer;
@end
static NSUInteger calls;
static NSUInteger observedHash(id object, SEL selector) {
  (void)object;
  (void)selector;
  return ++calls;
}
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif
int main(void) {
  Class cls = [NDSavedValues class];
  if (!class_addMethod(cls, @selector(hash), (IMP)observedHash,
                       method_getTypeEncoding(class_getInstanceMethod(
                           [NSObject class], @selector(hash)))))
    return 1;
#ifdef NEVERD_RECOVERED_ARC
  installRecovered();
#endif
  @autoreleasepool {
    NDSavedValues *object = [NDSavedValues new];
    for (uint32_t i = 0; i != 65536; ++i) {
      BOOL flag = i & 1;
      unsigned char byte = (unsigned char)i;
      unsigned short word = (unsigned short)i;
      uint32_t bits = i * UINT32_C(65537);
      int integer;
      memcpy(&integer, &bits, sizeof(integer));
      object.flag = flag;
      if (object.flag != flag || calls != 4 * i + 1)
        return 2;
      object.byte = byte;
      if (object.byte != byte || calls != 4 * i + 2)
        return 3;
      object.word = word;
      if (object.word != word || calls != 4 * i + 3)
        return 4;
      object.integer = integer;
      if (object.integer != integer || calls != 4 * i + 4)
        return 5;
    }
    [object release];
    puts("scalar-cases=262144\ncall-effects=262144\nknown-bytes=pass");
  }
  return 0;
}
