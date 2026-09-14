#import "ObjCCoreDataCalls.h"

@implementation NDCoreDataCalls
- (NSArray *)fetchFromContext:(NSManagedObjectContext *)context
                      request:(NSFetchRequest *)request
                        error:(NSError **)error {
  return [context executeFetchRequest:request error:error];
}
- (NSUInteger)countInContext:(NSManagedObjectContext *)context
                     request:(NSFetchRequest *)request
                       error:(NSError **)error {
  return [context countForFetchRequest:request error:error];
}
- (NSSet *)registeredObjectsInContext:(NSManagedObjectContext *)context {
  return context.registeredObjects;
}
@end
