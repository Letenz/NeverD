// Owned fixture: compile with ARC; rebuild recovered methods without ARC.
#import <Foundation/Foundation.h>

@interface NDARCBox : NSObject
@property(nonatomic, strong) id item;
@property(nonatomic, weak) id observer;
@property(nonatomic, copy) NSString *title;
@end

@implementation NDARCBox
@end
