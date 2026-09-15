#import "ObjCMetadataCalls.h"

extern NDMetadataResponse
nd_metadata_url(uintptr_t) __asm__("_$s10Foundation3URLVMa")
    __attribute__((swiftcall));
extern NDMetadataResponse
nd_metadata_date(uintptr_t) __asm__("_$s10Foundation4DateVMa")
    __attribute__((swiftcall));
extern NDMetadataResponse
nd_metadata_characterSet(uintptr_t) __asm__("_$s10Foundation12CharacterSetVMa")
    __attribute__((swiftcall));
extern NDMetadataResponse nd_metadata_dateComponents(uintptr_t) __asm__(
    "_$s10Foundation14DateComponentsVMa") __attribute__((swiftcall));
extern NDMetadataResponse
nd_metadata_request(uintptr_t) __asm__("_$s10Foundation10URLRequestVMa")
    __attribute__((swiftcall));
extern NDMetadataResponse
nd_metadata_indexPath(uintptr_t) __asm__("_$s10Foundation9IndexPathVMa")
    __attribute__((swiftcall));
extern NDMetadataResponse
nd_metadata_locale(uintptr_t) __asm__("_$s10Foundation6LocaleVMa")
    __attribute__((swiftcall));
extern NDMetadataResponse
nd_metadata_notification(uintptr_t) __asm__("_$s10Foundation12NotificationVMa")
    __attribute__((swiftcall));

@implementation NDMetadataCalls
- (NDMetadataResponse)url:(uintptr_t)request {
  return nd_metadata_url(request);
}
- (NDMetadataResponse)date:(uintptr_t)request {
  return nd_metadata_date(request);
}
- (NDMetadataResponse)characterSet:(uintptr_t)request {
  return nd_metadata_characterSet(request);
}
- (NDMetadataResponse)dateComponents:(uintptr_t)request {
  return nd_metadata_dateComponents(request);
}
- (NDMetadataResponse)request:(uintptr_t)request {
  return nd_metadata_request(request);
}
- (NDMetadataResponse)indexPath:(uintptr_t)request {
  return nd_metadata_indexPath(request);
}
- (NDMetadataResponse)locale:(uintptr_t)request {
  return nd_metadata_locale(request);
}
- (NDMetadataResponse)notification:(uintptr_t)request {
  return nd_metadata_notification(request);
}
@end
