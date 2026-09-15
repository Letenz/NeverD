#import <Foundation/Foundation.h>
#include <stdint.h>
typedef struct {
  void *object;
  void *storage;
} NDBoxPair;
typedef struct {
  const void *metadata;
  uintptr_t state;
} NDMetadataResponse;
extern NDBoxPair __attribute__((swiftcall)) swift_allocBox(const void *);
extern NDMetadataResponse __attribute__((swiftcall))
swift_checkMetadataState(uintptr_t, const void *);
@interface NDSwiftRecordRuntime : NSObject
- (void *)newBox:(const void *)metadata
           value:(NSUInteger)value
         storage:(void **)storage;
- (NSUInteger)metadataState:(const void *)metadata
                    request:(NSUInteger)request
                     result:(const void **)result;
@end
@implementation NDSwiftRecordRuntime
- (void *)newBox:(const void *)metadata
           value:(NSUInteger)value
         storage:(void **)storage {
  NDBoxPair pair = swift_allocBox(metadata);
  *(uintptr_t *)pair.storage = value;
  *storage = pair.storage;
  return pair.object;
}
- (NSUInteger)metadataState:(const void *)metadata
                    request:(NSUInteger)request
                     result:(const void **)result {
  NDMetadataResponse response = swift_checkMetadataState(request, metadata);
  *result = response.metadata;
  return response.state;
}
@end
