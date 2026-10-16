// Original dependent dylib: the main image also imports its base dependency.
extern __thread long base_tls;
extern long base_step(long);
extern long *base_address(void);
__thread long leaf_tls = 19;
// Same export spelling as the base library, with independent ownership.
long base_data = 99;

long leaf_step(void) {
  if (base_address() != &base_tls || base_data != 99)
    return -200;
  leaf_tls += 3;
  return base_step(5) + leaf_tls;
}
