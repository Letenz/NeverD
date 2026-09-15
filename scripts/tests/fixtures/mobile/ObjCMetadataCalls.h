#import <Foundation/Foundation.h>
#include <stdint.h>

typedef struct {
  const void *value;
  uintptr_t state;
} NDMetadataResponse;

@interface NDMetadataCalls : NSObject
- (NDMetadataResponse)url:(uintptr_t)request;
- (NDMetadataResponse)date:(uintptr_t)request;
- (NDMetadataResponse)characterSet:(uintptr_t)request;
- (NDMetadataResponse)dateComponents:(uintptr_t)request;
- (NDMetadataResponse)request:(uintptr_t)request;
- (NDMetadataResponse)indexPath:(uintptr_t)request;
- (NDMetadataResponse)locale:(uintptr_t)request;
- (NDMetadataResponse)notification:(uintptr_t)request;
@end
