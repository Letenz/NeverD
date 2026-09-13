// Owned fixture: exercise public runtime calls with caller-supplied keys.
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

@interface NDARCBox : NSObject
- (id)objectForKey:(const void *)key;
- (void)storeObject:(id)object
             forKey:(const void *)key
             policy:(NSUInteger)policy;
- (void)clearAssociatedObjects;
@end

@implementation NDARCBox
- (id)objectForKey:(const void *)key {
  return objc_getAssociatedObject(self, key);
}
- (void)storeObject:(id)object
             forKey:(const void *)key
             policy:(NSUInteger)policy {
  objc_setAssociatedObject(self, key, object, policy);
}
- (void)clearAssociatedObjects {
  objc_removeAssociatedObjects(self);
}
@end
