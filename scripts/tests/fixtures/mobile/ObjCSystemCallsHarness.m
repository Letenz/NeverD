#import "ObjCSystemCalls.h"

#include <errno.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

static BOOL containsName(const char *names, size_t size, const char *name) {
  for (size_t offset = 0; offset < size;) {
    size_t length = strnlen(names + offset, size - offset);
    if (length == size - offset)
      return NO;
    if (!strcmp(names + offset, name))
      return YES;
    offset += length + 1;
  }
  return NO;
}

static int exercise(const char *path) {
  @autoreleasepool {
    NDSystemCalls *object = [NDSystemCalls new];
    os_log_t log = os_log_create("org.neverd.fixture", "source");
    asl_object_t message = asl_new(ASL_TYPE_MSG);
    if (!message)
      return 1;
    const char *name = "org.neverd.source.fixture";
    unsigned char input[512], output[512];
    for (unsigned i = 0; i < sizeof(input); ++i)
      input[i] = (unsigned char)(i * 73 + 19);
    for (unsigned length = 0; length < sizeof(input); ++length) {
      if ([object setAttribute:path name:name bytes:input length:length])
        return 2;
      memset(output, 0xa5, sizeof(output));
      if ([object getAttribute:path
                          name:name
                         bytes:output
                        length:sizeof(output)] != length ||
          memcmp(input, output, length) || output[length] != 0xa5)
        return 3;
      char names[4096];
      ssize_t count = [object listAttributes:path
                                       bytes:names
                                      length:sizeof(names)];
      if (count < 0 || !containsName(names, (size_t)count, name))
        return 4;
      if ([object removeAttribute:path name:name] ||
          getxattr(path, name, output, sizeof(output), 0, 0) != -1 ||
          errno != ENOATTR)
        return 5;
      os_log_type_t type = length % 2 ? OS_LOG_TYPE_DEBUG : OS_LOG_TYPE_DEFAULT;
      if ([object logEnabled:log type:type] != os_log_type_enabled(log, type))
        return 6;
      char value[32];
      snprintf(value, sizeof(value), "value-%u", length);
      if ([object setMessage:message key:"FixtureValue" value:value])
        return 7;
      const char *stored = [object messageValue:message key:"FixtureValue"];
      if (!stored || strcmp(stored, value))
        return 8;
      unsigned char expected[CC_SHA256_DIGEST_LENGTH], actual[sizeof(expected)];
      CC_SHA256(input, length, expected);
      if ([object digest:input length:length output:actual] != actual ||
          memcmp(actual, expected, sizeof(actual)))
        return 9;
      CC_SHA256_CTX context;
      if (![object initializeDigest:&context] ||
          ![object updateDigest:&context bytes:input length:length / 2] ||
          ![object updateDigest:&context
                          bytes:input + length / 2
                         length:length - length / 2] ||
          ![object finishDigest:&context output:actual] ||
          memcmp(actual, expected, sizeof(actual)))
        return 10;
    }
    asl_release(message);
    os_release(log);
    [object release];
  }
  puts("system-c-cases=512\nextended-attribute-roundtrips=512\nlog-decisions="
       "512\nasl-roundtrips=512\nsha256-vectors=512");
  return 0;
}

int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NSString *template = [NSTemporaryDirectory()
        stringByAppendingPathComponent:@"neverd-system-c-XXXXXX"];
    char *path = strdup([template fileSystemRepresentation]);
    if (!path)
      return 11;
    int fd = mkstemp(path);
    if (fd < 0) {
      free(path);
      return 12;
    }
    close(fd);
    int status = exercise(path);
    unlink(path);
    free(path);
    return status;
  }
}
