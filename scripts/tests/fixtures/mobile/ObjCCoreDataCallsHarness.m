#import "ObjCCoreDataCalls.h"

#import <objc/runtime.h>
#include <stdio.h>

#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

int main(void) {
#ifdef NEVERD_RECOVERED_ARC
  installRecovered();
#endif
  @autoreleasepool {
    NSAttributeDescription *attribute = [NSAttributeDescription new];
    attribute.name = @"value";
    attribute.attributeType = NSInteger64AttributeType;
    NSEntityDescription *entity = [NSEntityDescription new];
    entity.name = @"Item";
    entity.managedObjectClassName = @"NSManagedObject";
    entity.properties = @[ attribute ];
    NSManagedObjectModel *model = [NSManagedObjectModel new];
    model.entities = @[ entity ];
    NSPersistentStoreCoordinator *coordinator =
        [[NSPersistentStoreCoordinator alloc] initWithManagedObjectModel:model];
    NSError *error = nil;
    if (![coordinator addPersistentStoreWithType:NSInMemoryStoreType
                                   configuration:nil
                                             URL:nil
                                         options:nil
                                           error:&error] ||
        error)
      return 1;
    NSManagedObjectContext *context = [[NSManagedObjectContext alloc]
        initWithConcurrencyType:NSMainQueueConcurrencyType];
    context.persistentStoreCoordinator = coordinator;
    NSMutableSet *expected = [NSMutableSet new];
    for (unsigned i = 0; i < 16; ++i) {
      NSManagedObject *object =
          [NSEntityDescription insertNewObjectForEntityForName:@"Item"
                                        inManagedObjectContext:context];
      [object setValue:@(i) forKey:@"value"];
      [expected addObject:object];
    }
    if (![context save:&error] || error)
      return 2;
    NDCoreDataCalls *driver = [NDCoreDataCalls new];
    NSFetchRequest *request =
        [NSFetchRequest fetchRequestWithEntityName:@"Item"];
    for (unsigned i = 0; i < 1024; ++i) {
      NSArray *objects = [driver fetchFromContext:context
                                          request:request
                                            error:&error];
      if (error || objects.count != 16 ||
          ![[NSSet setWithArray:objects] isEqualToSet:expected] ||
          [driver countInContext:context request:request error:&error] != 16 ||
          error ||
          ![[driver registeredObjectsInContext:context] isEqualToSet:expected])
        return 3;
    }
    if ([driver fetchFromContext:nil request:request error:&error] || error ||
        [driver countInContext:nil request:request error:&error] || error ||
        [driver registeredObjectsInContext:nil])
      return 4;
    puts("core-data-fetches=1024\ncontext-identity=pass\nfetch-count=16\n"
         "nil-context=pass");
    [driver release];
    [expected release];
    [context release];
    [coordinator release];
    [model release];
    [entity release];
    [attribute release];
  }
  return 0;
}
