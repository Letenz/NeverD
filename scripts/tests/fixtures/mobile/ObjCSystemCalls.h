#include <CommonCrypto/CommonDigest.h>
#import <Foundation/Foundation.h>
#include <asl.h>
#include <os/log.h>
#include <sys/xattr.h>

@interface NDSystemCalls : NSObject
- (int)setAttribute:(const char *)path
               name:(const char *)name
              bytes:(const void *)bytes
             length:(size_t)length;
- (ssize_t)getAttribute:(const char *)path
                   name:(const char *)name
                  bytes:(void *)bytes
                 length:(size_t)length;
- (ssize_t)listAttributes:(const char *)path
                    bytes:(char *)bytes
                   length:(size_t)length;
- (int)removeAttribute:(const char *)path name:(const char *)name;
- (BOOL)logEnabled:(os_log_t)log type:(os_log_type_t)type;
- (int)setMessage:(asl_object_t)message
              key:(const char *)key
            value:(const char *)value;
- (const char *)messageValue:(asl_object_t)message key:(const char *)key;
- (unsigned char *)digest:(const void *)bytes
                   length:(CC_LONG)length
                   output:(unsigned char *)output;
- (int)initializeDigest:(CC_SHA256_CTX *)context;
- (int)updateDigest:(CC_SHA256_CTX *)context
              bytes:(const void *)bytes
             length:(CC_LONG)length;
- (int)finishDigest:(CC_SHA256_CTX *)context output:(unsigned char *)output;
@end
