#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

@interface NDUnfairLocks : NSObject
- (void)add:(uint64_t)value;
- (uint64_t)value;
- (BOOL)tryAdd:(uint64_t)value;
- (void)lock;
- (void)unlock;
- (void)assertOwner;
- (void)assertNotOwner;
@end

#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

static void checked(int value, unsigned line) {
  if (!value) {
    fprintf(stderr, "unfair lock check failed at line %u\n", line);
    abort();
  }
}
#define check(value) checked(!!(value), __LINE__)

static void *increment(void *context) {
  @autoreleasepool {
    NDUnfairLocks *counter = (NDUnfairLocks *)context;
    for (unsigned i = 0; i < 4096; ++i)
      [counter add:1];
  }
  return NULL;
}

static void *tryWhileOwned(void *context) {
  @autoreleasepool {
    NDUnfairLocks *counter = (NDUnfairLocks *)context;
    [counter assertNotOwner];
    check(![counter tryAdd:100]);
  }
  return NULL;
}

int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    // NSObject allocation zeros the inline lock, matching OS_UNFAIR_LOCK_INIT.
    NDUnfairLocks *counter = [NDUnfairLocks new];
    check([counter value] == 0);
    [counter assertNotOwner];
    [counter lock];
    [counter assertOwner];
    pthread_t attempt;
    check(pthread_create(&attempt, NULL, tryWhileOwned, counter) == 0);
    check(pthread_join(attempt, NULL) == 0);
    [counter unlock];
    check([counter value] == 0);
    check([counter tryAdd:7]);
    check([counter value] == 7);
    pthread_t workers[4];
    for (unsigned i = 0; i < 4; ++i)
      check(pthread_create(&workers[i], NULL, increment, counter) == 0);
    for (unsigned i = 0; i < 4; ++i)
      check(pthread_join(workers[i], NULL) == 0);
    check([counter value] == 7 + 4 * 4096);
    [counter release];
    puts("unfair-locks=pass\ntrylock=pass\nownership=pass\nconcurrency=pass");
  }
}
