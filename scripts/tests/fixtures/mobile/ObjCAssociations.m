// Owned fixture: caller-supplied keys and two interior static key identities.
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

@interface NDARCBox : NSObject
- (id)objectForKey:(const void *)key;
- (void)storeObject:(id)object
             forKey:(const void *)key
             policy:(NSUInteger)policy;
- (void)clearAssociatedObjects;
- (id)objectForStaticKey;
- (void)storeObjectForStaticKey:(id)object;
- (id)objectForInteriorKey;
- (void)storeObjectForInteriorKey:(id)object;
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
- (id)objectForStaticKey {
  return objc_getAssociatedObject(self, &"neverd-key-storage"[1]);
}
- (void)storeObjectForStaticKey:(id)object {
  objc_setAssociatedObject(self, &"neverd-key-storage"[1], object,
                           OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
- (id)objectForInteriorKey {
  return objc_getAssociatedObject(self, &"neverd-key-storage"[2]);
}
- (void)storeObjectForInteriorKey:(id)object {
  objc_setAssociatedObject(self, &"neverd-key-storage"[2], object,
                           OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
@end
