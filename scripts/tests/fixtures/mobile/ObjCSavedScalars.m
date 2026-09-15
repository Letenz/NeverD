#import <Foundation/Foundation.h>
@interface NDSavedValues : NSObject {
  BOOL _flag;
  unsigned char _byte;
  unsigned short _word;
  int _integer;
}
@property(nonatomic) BOOL flag;
@property(nonatomic) unsigned char byte;
@property(nonatomic) unsigned short word;
@property(nonatomic) int integer;
@end
@implementation NDSavedValues
- (void)setFlag:(BOOL)value {
  (void)[self hash];
  _flag = value;
}
- (void)setByte:(unsigned char)value {
  (void)[self hash];
  _byte = value;
}
- (void)setWord:(unsigned short)value {
  (void)[self hash];
  _word = value;
}
- (void)setInteger:(int)value {
  (void)[self hash];
  _integer = value;
}
@end
