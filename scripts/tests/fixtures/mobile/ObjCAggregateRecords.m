#import "ObjCAggregateRecords.h"

@implementation NDRecords
- (NDPair)pair:(NDPair)value {
  return value;
}
- (NDQuad)quad:(NDQuad)value {
  return value;
}
- (NDQuad)rotate:(NDQuad)value {
  return (NDQuad){value.b, value.a};
}
- (NDQuad)throughCall:(NDQuad)value {
  return [self rotate:value];
}
- (NDQuad)scale:(NDQuad)value factor:(double)factor {
  return (NDQuad){{value.a.x * factor, value.a.y * factor},
                  {value.b.x * factor, value.b.y * factor}};
}
- (NDFloats)floats:(NDFloats)value {
  return value;
}
- (NDTriple)triple:(NDTriple)value {
  return value;
}
- (NDPair)stackA:(double)a
               b:(double)b
               c:(double)c
               d:(double)d
               e:(double)e
               f:(double)f
               g:(double)g
            pair:(NDPair)value
            tail:(double)tail {
  return (NDPair){value.x + a + b + c + d, value.y + e + f + g + tail};
}
@end
