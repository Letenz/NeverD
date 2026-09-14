#import <Foundation/Foundation.h>
#include <errno.h>
#include <math.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdio.h>

@interface NDDarwinDeclarations : NSObject
- (NSString *)nameOfClass:(Class)value;
- (Class)classNamed:(NSString *)value;
- (NSString *)nameOfSelector:(SEL)value;
- (SEL)selectorNamed:(NSString *)value;
- (int64_t)incrementWithLock:(pthread_mutex_t *)lock counter:(int64_t *)counter;
- (int64_t)incrementWithObject:(id)object counter:(int64_t *)counter;
- (int64_t)compare:(CFStringRef)left with:(CFStringRef)right;
- (uint64_t)time:(uint64_t)when delta:(int64_t)delta;
- (int)lastError;
- (double)remainder:(double)value divisor:(double)divisor;
- (NSString *)defaultMode;
- (const void *)modeStorage;
- (NSString *)descriptionKey;
- (void *)mainQueue;
- (const void *)timerType;
- (float)defaultPriority;
- (double)foundationVersion;
@end
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif
struct Worker {
  NDDarwinDeclarations *driver;
  pthread_mutex_t mutex;
  int64_t locked, synchronized;
};
static void *work(void *context) {
  struct Worker *worker = context;
  @autoreleasepool {
    for (unsigned i = 0; i < 2048; ++i) {
      [worker->driver incrementWithLock:&worker->mutex counter:&worker->locked];
      [worker->driver incrementWithObject:worker->driver
                                  counter:&worker->synchronized];
    }
  }
  return NULL;
}
int main(void) {
#ifdef NEVERD_RECOVERED_ARC
  installRecovered();
#endif
  @autoreleasepool {
    NDDarwinDeclarations *driver = [NDDarwinDeclarations new];
    for (unsigned i = 0; i < 2048; ++i) {
      if (![[driver nameOfClass:[NSMutableArray class]]
              isEqualToString:@"NSMutableArray"] ||
          [driver classNamed:@"NSMutableArray"] != [NSMutableArray class] ||
          [driver classNamed:@"NDMissingClass"] != Nil)
        return 1;
      SEL selector = @selector(incrementWithLock:counter:);
      if (![[driver nameOfSelector:selector]
              isEqualToString:@"incrementWithLock:counter:"] ||
          [driver selectorNamed:@"incrementWithLock:counter:"] != selector)
        return 2;
      if ([driver compare:CFSTR("AbC") with:CFSTR("abc")] != 0 ||
          [driver compare:CFSTR("a") with:CFSTR("b")] >= 0 ||
          [driver compare:CFSTR("z") with:CFSTR("b")] <= 0)
        return 3;
      uint64_t when = UINT64_C(1000000000000) + i;
      int64_t delta = -(int64_t)i * 31;
      if ([driver time:when delta:delta] != dispatch_time(when, delta))
        return 4;
      errno = i & 127;
      if ([driver lastError] != (int)(i & 127))
        return 5;
      if ([driver defaultMode] != NSDefaultRunLoopMode ||
          [driver modeStorage] != &NSDefaultRunLoopMode ||
          [driver descriptionKey] != NSLocalizedDescriptionKey ||
          [driver mainQueue] != (void *)dispatch_get_main_queue() ||
          [driver timerType] != DISPATCH_SOURCE_TYPE_TIMER ||
          [driver defaultPriority] != NSURLSessionTaskPriorityDefault ||
          [driver foundationVersion] != NSFoundationVersionNumber)
        return 10;
      double value = -12.25 * i;
      if ([driver remainder:value divisor:3.125] != fmod(value, 3.125))
        return 6;
    }
    struct Worker worker = {driver, PTHREAD_MUTEX_INITIALIZER, 0, 0};
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; ++i)
      if (pthread_create(&threads[i], NULL, work, &worker))
        return 7;
    for (unsigned i = 0; i < 4; ++i)
      if (pthread_join(threads[i], NULL))
        return 8;
    if (worker.locked != 8192 || worker.synchronized != 8192)
      return 9;
    pthread_mutex_destroy(&worker.mutex);
    [driver release];
  }
  puts("darwin-declarations=2048\nlocked-updates=8192\nsynchronized-updates="
       "8192");
  return 0;
}
