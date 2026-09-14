#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern long __stack_chk_guard[8];
extern void NDSetStackFailureReceipt(int fd);

@interface NDProtocolCalls : NSObject
- (NSUInteger)enumerate:(id<NSFastEnumeration>)source
                  state:(NSFastEnumerationState *)state
                objects:(id __unsafe_unretained *)objects
                  count:(NSUInteger)count;
- (uint64_t)metricOf:(id)source;
- (BOOL)isNegativeMetric:(id)source;
- (NSUInteger)countObjects:(id<NSFastEnumeration>)source;
- (NSUInteger)reportMutation:(id)object;
- (NSUInteger)checkRuntimeGuard:(uintptr_t)expected;
@end

@interface NDChangingCollection : NSObject <NSFastEnumeration> {
  unsigned long generation;
}
@end
@implementation NDChangingCollection
- (NSUInteger)countByEnumeratingWithState:(NSFastEnumerationState *)state
                                  objects:(id __unsafe_unretained *)objects
                                    count:(NSUInteger)capacity {
  if (state->state >= 7 || !capacity)
    return 0;
  const NSUInteger count = MIN(MIN(capacity, 3), 7 - state->state);
  for (NSUInteger index = 0; index < count; ++index)
    objects[index] = self;
  state->itemsPtr = objects;
  state->mutationsPtr = &generation;
  state->state += count;
  ++generation;
  return count;
}
@end

@interface NDMetricProvider : NSObject {
@public
  uint64_t bits;
}
- (int64_t)metric;
@end
@implementation NDMetricProvider
- (int64_t)metric {
  return (int64_t)bits;
}
@end

#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

static id mutationObject;
static NSUInteger mutations;
static void recordMutation(id object) {
  mutationObject = object;
  ++mutations;
}

int main(int argc, char **argv) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NDProtocolCalls *calls = [NDProtocolCalls new];
    if (argc == 3 && !strcmp(argv[1], "--guard-fail")) {
      NDSetStackFailureReceipt(atoi(argv[2]));
      [calls checkRuntimeGuard:(uintptr_t)__stack_chk_guard[0] ^ 1];
      return 13;
    }
    if ([calls checkRuntimeGuard:(uintptr_t)__stack_chk_guard[0]] != 73)
      return 14;
    NSUInteger visited = 0;
    for (NSUInteger length = 0; length <= 128; ++length) {
      NSMutableArray *source = [NSMutableArray array];
      for (NSUInteger index = 0; index < length; ++index)
        [source addObject:@(index)];
      if ([calls countObjects:source] != length)
        return 8;
      for (NSUInteger capacity = 1; capacity <= 16; ++capacity) {
        NSFastEnumerationState state = {0};
        id __unsafe_unretained buffer[16];
        NSUInteger total = 0;
        while (true) {
          NSUInteger count = [calls enumerate:source
                                        state:&state
                                      objects:buffer
                                        count:capacity];
          if (count == 0)
            break;
          if (count > length - total || !state.itemsPtr || !state.mutationsPtr)
            return 2;
          for (NSUInteger index = 0; index < count; ++index)
            if (![state.itemsPtr[index] isEqual:@(total + index)])
              return 3;
          total += count;
          if (total > length)
            return 4;
        }
        if (total != length)
          return 5;
        visited += total;
      }
    }
    printf("enumerated=%lu\n", (unsigned long)visited);
    objc_setEnumerationMutationHandler(recordMutation);
    for (NSUInteger index = 0; index < 2048; ++index) {
      id object = index & 1 ? calls : @"mutation";
      if ([calls reportMutation:object] != 73 || mutationObject != object ||
          mutations != index + 1)
        return 9;
    }
    printf("mutations=2048\n");
    NDChangingCollection *changing = [NDChangingCollection new];
    if ([calls countObjects:changing] != 7 || mutations != 2052 ||
        mutationObject != changing)
      return 10;
    [changing release];
    objc_setEnumerationMutationHandler(NULL);
    printf("loop-mutations=4\n");
    NDMetricProvider *provider = [NDMetricProvider new];
    uint64_t state = 0x931492a5572efd81;
    for (unsigned index = 0; index < 4096; ++index) {
      const uint64_t edges[] = {0, 1, INT64_MAX, UINT64_C(1) << 63, UINT64_MAX};
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
      provider->bits = index < 5 ? edges[index] : state;
      if ([calls metricOf:provider] != provider->bits)
        return 6;
      if (!![calls isNegativeMetric:provider] != !!(provider->bits >> 63))
        return 7;
    }
    [provider release];
    [calls release];
    printf("integer-bits=4096\n");
    int receiptPipe[2];
    if (pipe(receiptPipe))
      return 15;
    char descriptor[32];
    snprintf(descriptor, sizeof(descriptor), "%d", receiptPipe[1]);
    pid_t child = fork();
    if (child < 0)
      return 16;
    if (!child) {
      close(receiptPipe[0]);
      execl(argv[0], argv[0], "--guard-fail", descriptor, NULL);
      _exit(17);
    }
    close(receiptPipe[1]);
    int status;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status))
      return 18;
    unsigned char receipt = 0;
    const ssize_t received = read(receiptPipe[0], &receipt, 1);
    close(receiptPipe[0]);
    if (received != 1 || receipt != 93 || WEXITSTATUS(status) != receipt)
      return 19;
    puts("guard-check=pass");
  }
  return 0;
}
