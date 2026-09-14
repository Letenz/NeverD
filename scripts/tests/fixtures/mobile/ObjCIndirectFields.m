#import "ObjCIndirectFields.h"

#include <stdint.h>

extern uint32_t firstOffset __asm__("_OBJC_IVAR_$_NDIndirectFields._first");
extern uint32_t secondOffset __asm__("_OBJC_IVAR_$_NDIndirectFields._second");
extern uintptr_t NDKeepField(void *) __asm__("_objc_retain");

// Keep a shared native consumer between the caller and the runtime offset.
// The pointed-to scalar is read exactly once before any other memory effect.
__attribute__((noinline, disable_tail_calls)) uintptr_t
NDReadObjectField(uintptr_t object, const uint32_t *offset) {
  const uint32_t displacement = *offset;
  return NDKeepField(*(void **)(object + displacement));
}

@implementation NDIndirectFields
- (id)first {
  return (__bridge_transfer id)(void *)NDReadObjectField(
      (uintptr_t)(__bridge void *)self, &firstOffset);
}
- (id)second {
  return (__bridge_transfer id)(void *)NDReadObjectField(
      (uintptr_t)(__bridge void *)self, &secondOffset);
}
- (void)setFirst:(id)value {
  _first = value;
}
- (void)setSecond:(id)value {
  _second = value;
}
@end
