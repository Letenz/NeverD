typedef unsigned int u32;
typedef unsigned long long u64;

__attribute__((noinline)) u64 mba_five_sum(u64 x, u64 y, u64 z, u64 w, u64 v) {
  u64 first_parity = x ^ y ^ z;
  u64 first_majority = (x & y) | (x & z) | (y & z);
  u64 second_parity = first_parity ^ w ^ v;
  u64 second_majority = (first_parity & w) | (first_parity & v) | (w & v);
  return second_parity + (second_majority << 1) + (first_majority << 1);
}

u32 mba_five_fold(u32 xl, u32 xh, u32 yl, u32 yh, u32 zl, u32 zh, u32 wl,
                  u32 wh, u32 vl, u32 vh) {
  u64 x = ((u64)xh << 32) | xl;
  u64 y = ((u64)yh << 32) | yl;
  u64 z = ((u64)zh << 32) | zl;
  u64 w = ((u64)wh << 32) | wl;
  u64 v = ((u64)vh << 32) | vl;
  u64 sum = mba_five_sum(x, y, z, w, v);
  return (u32)sum ^ (u32)(sum >> 32);
}
