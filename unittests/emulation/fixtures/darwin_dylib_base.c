// Original SDK-free startup-dylib and C TLS workload.
extern int getpid(void);
long base_data = 11;
long *base_pointer = &base_data;
__thread long base_tls;
__thread unsigned char base_zero[19];

long *base_address(void) { return &base_tls; }
unsigned char *base_zero_address(void) { return base_zero; }
long base_step(long delta) {
  if (base_pointer != &base_data || *base_pointer != 11 || getpid() <= 0)
    return -100;
  base_tls += delta;
  ++base_zero[3];
  return base_tls;
}
