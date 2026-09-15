#import <Foundation/Foundation.h>

typedef struct NDPair {
  double x, y;
} NDPair;
typedef struct NDQuad {
  NDPair a, b;
} NDQuad;
typedef struct NDFloats {
  float a, b, c, d;
} NDFloats;
typedef struct NDTriple {
  float a, b, c;
} NDTriple;
@interface NDRecords : NSObject
- (NDPair)pair:(NDPair)value;
- (NDQuad)quad:(NDQuad)value;
- (NDQuad)rotate:(NDQuad)value;
- (NDQuad)throughCall:(NDQuad)value;
- (NDQuad)scale:(NDQuad)value factor:(double)factor;
- (NDFloats)floats:(NDFloats)value;
- (NDTriple)triple:(NDTriple)value;
- (NDPair)stackA:(double)a
               b:(double)b
               c:(double)c
               d:(double)d
               e:(double)e
               f:(double)f
               g:(double)g
            pair:(NDPair)value
            tail:(double)tail;
@end
