#import "ObjCAggregateRecords.h"

#import <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif
int main(void) {
#ifdef NEVERD_RECOVERED_ARC
  installRecovered();
#endif
  @autoreleasepool {
    NDRecords *object = [NDRecords new];
    const uint64_t cases[] = {0,
                              UINT64_C(0x8000000000000000),
                              UINT64_C(0x7ff0000000000000),
                              UINT64_C(0xfff0000000000000),
                              UINT64_C(0x7ff8123456789abc),
                              UINT64_C(0xfff0abcd12345678),
                              UINT64_C(0x0000000000000001),
                              UINT64_C(0x3ff0000000000000)};
    const uint32_t fcases[] = {0,          0x80000000, 0x7f800000, 0xff800000,
                               0x7fc12345, 0xff812345, 1,          0x3f800000};
    for (unsigned i = 0; i < 1024; ++i) {
      uint64_t bits[4];
      uint32_t fbits[4];
      for (unsigned j = 0; j < 4; ++j) {
        bits[j] = cases[(i + j * 3) % 8] ^ (i > 128 ? (uint64_t)i << 17 : 0);
        fbits[j] = fcases[(i + j * 3) % 8] ^ (i > 128 ? i : 0);
      }
      NDQuad q;
      memcpy(&q, bits, sizeof(q));
      NDPair p = [object pair:q.a];
      if (memcmp(&p, &q.a, sizeof(p)))
        return 1;
      NDQuad out = [object quad:q];
      if (memcmp(&q, &out, sizeof(q)))
        return 2;
      NDQuad expected = {q.b, q.a};
      out = [object rotate:q];
      if (memcmp(&expected, &out, sizeof(out)))
        return 3;
      out = [object throughCall:q];
      if (memcmp(&expected, &out, sizeof(out)))
        return 4;
      NDFloats floats;
      memcpy(&floats, fbits, sizeof(floats));
      NDFloats fout = [object floats:floats];
      if (memcmp(&floats, &fout, sizeof(fout)))
        return 5;
      NDTriple triple;
      memcpy(&triple, fbits, sizeof(triple));
      NDTriple tout = [object triple:triple];
      if (memcmp(&triple, &tout, sizeof(tout)))
        return 6;
      double factor = (int)(i % 9) - 4.5;
      q = (NDQuad){{(int)i - 512, 2.25 * i}, {-3.5 * i, i / 8.0}};
      expected = (NDQuad){{q.a.x * factor, q.a.y * factor},
                          {q.b.x * factor, q.b.y * factor}};
      out = [object scale:q factor:factor];
      if (memcmp(&expected, &out, sizeof(out)))
        return 7;
      NDPair sp = [object stackA:1 b:2 c:3 d:4 e:5 f:6 g:7 pair:q.a tail:8];
      NDPair se = {q.a.x + 1 + 2 + 3 + 4, q.a.y + 5 + 6 + 7 + 8};
      if (memcmp(&sp, &se, sizeof(sp)))
        return 8;
    }
    [object release];
    puts("record-checks=8192\nrecord-bits=pass\nrecord-calls=pass\nrecord-"
         "stack=pass");
  }
  return 0;
}
