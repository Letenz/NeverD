/* Owned control-flow comparison fixture. Compile and inspect; never execute. */
#ifndef FINAL_ADD
#define FINAL_ADD 3u
#endif
#ifndef FAMILY
#define FAMILY 1
#endif
__attribute__((noinline, used)) unsigned transform(unsigned x, unsigned y) {
#if FAMILY == 1
  if (x < y)
    x = (x ^ 23u) * 7u + 9u;
  else
    x = (x + 17u) ^ (y * 13u);
#else
  unsigned n = y & 7u;
  while (n--) {
    x = (x ^ 23u) * 7u + 9u;
    x = (x >> 5) | (x << 27);
  }
#endif
  return (x ^ y) + FINAL_ADD;
}
