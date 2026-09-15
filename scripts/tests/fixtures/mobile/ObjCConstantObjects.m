#import <Foundation/Foundation.h>
#include <stdint.h>

@interface NDConstantObjects : NSObject
- (NSArray *)words;
- (NSArray *)nested;
- (NSNumber *)signedNumber;
- (NSNumber *)unsignedNumber;
- (NSDictionary *)mapping;
@end
@implementation NDConstantObjects
- (NSArray *)words {
  return @[ @"alpha", @"βeta", @"alpha" ];
}
- (NSArray *)nested {
  return @[ @[ @"alpha", @"βeta" ], @[ @"tail" ], @(-12345LL) ];
}
- (NSNumber *)signedNumber {
  return @(-12345LL);
}
- (NSNumber *)unsignedNumber {
  return @(UINT64_C(0xfedcba9876543210));
}
- (NSDictionary *)mapping {
  return @{@"first" : @[ @"alpha", @"βeta" ], @"second" : @(-12345LL)};
}
@end
