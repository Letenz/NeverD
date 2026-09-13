#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdio.h>

@interface NDARCBox : NSObject
- (id)objectForKey:(const void *)key;
- (void)storeObject:(id)object
             forKey:(const void *)key
             policy:(NSUInteger)policy;
- (void)clearAssociatedObjects;
@end

static int Destroyed;
@interface NDTracked : NSObject
@end
@implementation NDTracked
- (void)dealloc {
  ++Destroyed;
  [super dealloc];
}
@end

#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

int main(void) {
  NSAutoreleasePool *Pool = [NSAutoreleasePool new];
#ifdef NEVERD_RECOVERED_ARC
  installRecovered();
#endif
  static int ObjectKey, CopyKey;
  NDARCBox *Box = [NDARCBox new];
  NDTracked *Object = [NDTracked new];
  [Box storeObject:Object
            forKey:&ObjectKey
            policy:OBJC_ASSOCIATION_RETAIN_NONATOMIC];
  [Object release];
  if (Destroyed)
    return 1;
  NSAutoreleasePool *ReadPool = [NSAutoreleasePool new];
  if ([Box objectForKey:&ObjectKey] != Object ||
      [Box objectForKey:&CopyKey] != nil)
    return 2;
  NSMutableString *Text = [NSMutableString stringWithString:@"snapshot"];
  [Box storeObject:Text forKey:&CopyKey policy:OBJC_ASSOCIATION_COPY_NONATOMIC];
  [Text appendString:@"-changed"];
  if (![[Box objectForKey:&CopyKey] isEqualToString:@"snapshot"])
    return 3;
  [ReadPool drain];
  [Box clearAssociatedObjects];
  if (Destroyed != 1 || [Box objectForKey:&ObjectKey] != nil ||
      [Box objectForKey:&CopyKey] != nil)
    return 4;
  [Box release];
  [Pool drain];
  puts("associations=pass\nretain=pass\ncopy=pass\nclear=pass\ndestroyed=1");
  return 0;
}
