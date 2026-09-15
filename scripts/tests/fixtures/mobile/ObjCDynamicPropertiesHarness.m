#import "ObjCDynamicProperties.h"

#import <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>

#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

#ifdef NEVERD_STANDALONE_SOURCE
// The project exporter currently omits subclasses of external non-Foundation
// classes. Supply only the Core Data dependency's declaration metadata here;
// all six driver method bodies still come exclusively from the export.
@implementation NDDynamicRecord
@dynamic ndEventCount, ndWeight, ndLabel;
@end
#endif

int main(void) {
#ifdef NEVERD_RECOVERED_ARC
  installRecovered();
#endif
  @autoreleasepool {
    NSArray *names = @[ @"ndEventCount", @"ndWeight", @"ndLabel" ];
    NSAttributeType types[] = {NSInteger32AttributeType, NSDoubleAttributeType,
                               NSStringAttributeType};
    NSMutableArray *attributes = [NSMutableArray new];
    for (unsigned i = 0; i != 3; ++i) {
      NSAttributeDescription *attribute = [NSAttributeDescription new];
      attribute.name = names[i];
      attribute.attributeType = types[i];
      attribute.optional = YES;
      [attributes addObject:attribute];
      [attribute release];
    }
    NSEntityDescription *entity = [NSEntityDescription new];
    entity.name = @"Record";
    entity.managedObjectClassName = @"NDDynamicRecord";
    entity.properties = attributes;
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
    NDDynamicRecord *record =
        [NSEntityDescription insertNewObjectForEntityForName:@"Record"
                                      inManagedObjectContext:context];
    NDPropertyDriver *driver = [NDPropertyDriver new];
    for (int i = 0; i != 1024; ++i) {
      @autoreleasepool {
        int32_t count = i == 0   ? INT32_MIN
                        : i == 1 ? INT32_MAX
                                 : (i - 512) * 31337;
        double weight = (i - 512) / 8.0;
        NSString *label = [NSString stringWithFormat:@"record-%d-%d", i, count];
        [driver setEventCount:count record:record];
        [driver setWeight:weight record:record];
        [driver setLabel:label record:record];
        // KVC exercises Core Data independently of the reconstructed selectors.
        if ([[record valueForKey:@"ndEventCount"] intValue] != count ||
            [[record valueForKey:@"ndWeight"] doubleValue] != weight ||
            ![[record valueForKey:@"ndLabel"] isEqualToString:label])
          return 2;
        [record setValue:@(-i) forKey:@"ndEventCount"];
        [record setValue:@(-weight) forKey:@"ndWeight"];
        [record setValue:label forKey:@"ndLabel"];
        if ([driver eventCount:record] != -i ||
            [driver weight:record] != -weight ||
            [driver label:record] != [record valueForKey:@"ndLabel"])
          return 3;
      }
    }
    puts("dynamic-property-checks=6144\nscalar-widths=pass\nobject-identity="
         "pass");
    [driver release];
    [context release];
    [coordinator release];
    [model release];
    [entity release];
    [attributes release];
  }
  return 0;
}
