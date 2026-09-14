#import <Foundation/Foundation.h>

@interface NDIndirectFields : NSObject {
  id _first;
  id _second;
}
- (id)first;
- (id)second;
- (void)setFirst:(id)value;
- (void)setSecond:(id)value;
@end
