#import <Foundation/Foundation.h>

@interface NDInvariantLoops : NSObject
- (NSUInteger)nestedNumbers:(NSArray *)groups;
- (NSUInteger)nestedStrings:(NSArray *)groups;
@end
