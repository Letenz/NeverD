#import "ObjCProtocolReferences.h"

#import <objc/runtime.h>
#include <stdio.h>

#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

@interface NDProtocolValue : NSObject <NDValueProtocol>
@end
@implementation NDProtocolValue
- (id)value {
  return self;
}
@end

int main(void) {
#ifdef NEVERD_RECOVERED_ARC
  installRecovered();
#endif
  @autoreleasepool {
    NDProtocolReferences *object = [[NDProtocolReferences alloc] init];
    NDProtocolValue *value = [[NDProtocolValue alloc] init];
    NSObject *plain = [[NSObject alloc] init];
    for (unsigned i = 0; i < 1024; ++i) {
      Protocol *protocol = [object valueProtocol];
      if (!protocol || protocol != objc_getProtocol("NDValueProtocol") ||
          !protocol_isEqual(protocol, @protocol(NDValueProtocol)) ||
          [object rootProtocol] != objc_getProtocol("NSObject") ||
          [object sameValueProtocol] != protocol ||
          [object sameRootProtocol] != [object rootProtocol] ||
          ![value conformsToProtocol:protocol] ||
          [plain conformsToProtocol:protocol] ||
          ![value conformsToProtocol:[object rootProtocol]] ||
          ![plain conformsToProtocol:[object rootProtocol]])
        return 1;
    }
    [plain release];
    [value release];
    [object release];
  }
  puts("protocol-references=8192\nregistered-identity=pass\nconformance=pass");
  return 0;
}
