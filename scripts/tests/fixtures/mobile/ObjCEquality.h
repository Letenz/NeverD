#import <Foundation/Foundation.h>

@interface NDEquality : NSObject
@property(nonatomic, strong) id leftValue;
@property(nonatomic, strong) id rightValue;
- (BOOL)equivalent:(NDEquality *)other;
@end
