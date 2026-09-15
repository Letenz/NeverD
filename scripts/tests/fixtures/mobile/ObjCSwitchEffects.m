#import <Foundation/Foundation.h>

@interface NDSwitchEffects : NSObject
- (NSUInteger)choose:(NSUInteger)selector
              object:(id)value
              output:(NSUInteger *)output;
@end
@implementation NDSwitchEffects
- (NSUInteger)choose:(NSUInteger)selector
              object:(id)value
              output:(NSUInteger *)output {
  switch (selector) {
  case 16:
  case 0:
    *output = 17U;
    return [value hash] + 11U;
  case 1:
    *output = 118U;
    return [value hash] ^ 20U;
  case 2:
    *output = 219U;
    return [value hash] * 5U;
  case 3:
    *output = 320U;
    return ([value hash] << 4) | 4U;
  case 4:
    *output = 421U;
    return [value hash] + 15U;
  case 5:
    *output = 522U;
    return [value hash] ^ 24U;
  case 6:
    *output = 623U;
    return [value hash] * 9U;
  default:
  case 7:
    *output = 724U;
    return ([value hash] << 3) | 8U;
  case 8:
    *output = 825U;
    return [value hash] + 19U;
  case 9:
    *output = 926U;
    return [value hash] ^ 28U;
  case 10:
    *output = 1027U;
    return [value hash] * 13U;
  case 11:
    *output = 1128U;
    return ([value hash] << 2) | 12U;
  case 12:
    *output = 1229U;
    return [value hash] + 23U;
  case 13:
    *output = 1330U;
    return [value hash] ^ 32U;
  case 14:
    *output = 1431U;
    return [value hash] * 17U;
  case 15:
    *output = 1532U;
    return ([value hash] << 1) | 16U;
  }
}
@end
