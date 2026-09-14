#import <Foundation/Foundation.h>
#include <os/lock.h>

@interface NDUnfairLocks : NSObject {
  os_unfair_lock _lock;
  uint64_t _value;
}
- (void)add:(uint64_t)value;
- (uint64_t)value;
- (BOOL)tryAdd:(uint64_t)value;
- (void)lock;
- (void)unlock;
- (void)assertOwner;
- (void)assertNotOwner;
@end

@implementation NDUnfairLocks
- (void)add:(uint64_t)value {
  os_unfair_lock_assert_not_owner(&_lock);
  os_unfair_lock_lock(&_lock);
  os_unfair_lock_assert_owner(&_lock);
  _value += value;
  os_unfair_lock_unlock(&_lock);
}
- (uint64_t)value {
  os_unfair_lock_lock(&_lock);
  uint64_t result = _value;
  os_unfair_lock_unlock(&_lock);
  return result;
}
- (BOOL)tryAdd:(uint64_t)value {
  if (!os_unfair_lock_trylock(&_lock))
    return NO;
  _value += value;
  os_unfair_lock_unlock(&_lock);
  return YES;
}
- (void)lock {
  os_unfair_lock_lock(&_lock);
}
- (void)unlock {
  os_unfair_lock_unlock(&_lock);
}
- (void)assertOwner {
  os_unfair_lock_assert_owner(&_lock);
}
- (void)assertNotOwner {
  os_unfair_lock_assert_not_owner(&_lock);
}
@end
