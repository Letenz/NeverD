#import <CoreData/CoreData.h>

@interface NDCoreDataCalls : NSObject
- (NSArray *)fetchFromContext:(NSManagedObjectContext *)context
                      request:(NSFetchRequest *)request
                        error:(NSError **)error;
- (NSUInteger)countInContext:(NSManagedObjectContext *)context
                     request:(NSFetchRequest *)request
                       error:(NSError **)error;
- (NSSet *)registeredObjectsInContext:(NSManagedObjectContext *)context;
@end
