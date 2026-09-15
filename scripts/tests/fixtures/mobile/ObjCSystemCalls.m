#import "ObjCSystemCalls.h"

@implementation NDSystemCalls
- (int)setAttribute:(const char *)path
               name:(const char *)name
              bytes:(const void *)bytes
             length:(size_t)length {
  return setxattr(path, name, bytes, length, 0, 0);
}
- (ssize_t)getAttribute:(const char *)path
                   name:(const char *)name
                  bytes:(void *)bytes
                 length:(size_t)length {
  return getxattr(path, name, bytes, length, 0, 0);
}
- (ssize_t)listAttributes:(const char *)path
                    bytes:(char *)bytes
                   length:(size_t)length {
  return listxattr(path, bytes, length, 0);
}
- (int)removeAttribute:(const char *)path name:(const char *)name {
  return removexattr(path, name, 0);
}
- (BOOL)logEnabled:(os_log_t)log type:(os_log_type_t)type {
  return os_log_type_enabled(log, type);
}
- (int)setMessage:(asl_object_t)message
              key:(const char *)key
            value:(const char *)value {
  return asl_set(message, key, value);
}
- (const char *)messageValue:(asl_object_t)message key:(const char *)key {
  return asl_get(message, key);
}
- (unsigned char *)digest:(const void *)bytes
                   length:(CC_LONG)length
                   output:(unsigned char *)output {
  return CC_SHA256(bytes, length, output);
}
- (int)initializeDigest:(CC_SHA256_CTX *)context {
  return CC_SHA256_Init(context);
}
- (int)updateDigest:(CC_SHA256_CTX *)context
              bytes:(const void *)bytes
             length:(CC_LONG)length {
  return CC_SHA256_Update(context, bytes, length);
}
- (int)finishDigest:(CC_SHA256_CTX *)context output:(unsigned char *)output {
  return CC_SHA256_Final(output, context);
}
@end
