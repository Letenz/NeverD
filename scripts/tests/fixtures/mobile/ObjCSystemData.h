#import <CoreData/CoreData.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreSpotlight/CoreSpotlight.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

@interface NDSystemData : NSObject
- (NSArray *)emptyArray;
- (NSArray *)emptyArrayAlias;
- (NSDictionary *)emptyDictionary;
- (NSNumber *)trueObject;
- (NSNumber *)falseObject;
- (NSString *)contextSaveName;
- (NSString *)gifDictionaryKey;
- (NSString *)searchableItemIdentifier;
- (NSString *)colorSpaceName;
@end
