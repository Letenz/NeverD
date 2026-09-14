#import "ObjCSystemData.h"

@implementation NDSystemData
- (NSArray *)emptyArray {
  return @[];
}
- (NSArray *)emptyArrayAlias {
  return @[];
}
- (NSDictionary *)emptyDictionary {
  return @{};
}
- (NSNumber *)trueObject {
  return @YES;
}
- (NSNumber *)falseObject {
  return @NO;
}
- (NSString *)contextSaveName {
  return NSManagedObjectContextDidSaveNotification;
}
- (NSString *)gifDictionaryKey {
  return (__bridge NSString *)kCGImagePropertyGIFDictionary;
}
- (NSString *)searchableItemIdentifier {
  return CSSearchableItemActivityIdentifier;
}
- (NSString *)colorSpaceName {
  return (__bridge NSString *)kCGColorSpaceSRGB;
}
@end
