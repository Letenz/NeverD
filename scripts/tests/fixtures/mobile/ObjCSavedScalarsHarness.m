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
- (int)promoteByte:(unsigned char)value;
- (int)promoteSignedByte:(signed char)value;
- (int)promoteWord:(unsigned short)value;
- (int)promoteSignedWord:(short)value;
- (NSUInteger)branchForFlag:(BOOL)value;
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
      if (object.flag != flag || calls != 9 * i + 1)
        return 2;
      object.byte = byte;
      if (object.byte != byte || calls != 9 * i + 2)
        return 3;
      object.word = word;
      if (object.word != word || calls != 9 * i + 3)
        return 4;
      object.integer = integer;
      if (object.integer != integer || calls != 9 * i + 4)
        return 5;
      signed char signedByte;
      short signedWord;
      memcpy(&signedByte, &byte, sizeof(signedByte));
      memcpy(&signedWord, &word, sizeof(signedWord));
      if ([object promoteByte:byte] != (int)byte || calls != 9 * i + 5)
        return 6;
      if ([object promoteSignedByte:signedByte] != (int)signedByte ||
          calls != 9 * i + 6)
        return 7;
      if ([object promoteWord:word] != (int)word || calls != 9 * i + 7)
        return 8;
      if ([object promoteSignedWord:signedWord] != (int)signedWord ||
          calls != 9 * i + 8)
        return 9;
      if ([object branchForFlag:flag] != (flag ? 13U : 7U) ||
          calls != 9 * i + 9)
        return 10;
    }
    [object release];
    puts("scalar-cases=589824\ncall-effects=589824\nknown-bytes=pass");
  }
  return 0;
}
