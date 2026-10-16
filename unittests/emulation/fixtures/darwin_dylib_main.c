// Original SDK-free workload shared by native and modeled execution.
extern long write(int, const void *, unsigned long);
extern long base_data;
extern long *base_pointer;
extern __thread long base_tls;
extern __thread long leaf_tls;
extern long *base_address(void);
extern unsigned char *base_zero_address(void);
extern long base_step(long);
extern long leaf_step(void);
static __thread long main_tls = 23;
static __thread unsigned char main_zero[33] __attribute__((aligned(16)));

// Observe the original live template before its first descriptor access.
extern volatile long template_value __asm("_main_tls$tlv$init");
extern volatile unsigned char template_zero[33] __asm("_main_zero$tlv$init");
int main(void) {
  template_value = 31;
  template_zero[10] = 0x52;
  if (base_data != 11 || base_pointer != &base_data || base_tls != 0 ||
      leaf_tls != 19 || main_tls != 31 || base_address() != &base_tls ||
      (unsigned long)main_zero % 16)
    return 1;
  for (unsigned i = 0; i < sizeof(main_zero); ++i)
    if (main_zero[i] != (i == 10 ? 0x52 : 0))
      return 2;
  unsigned char *zero = base_zero_address();
  for (unsigned i = 0; i < 19; ++i)
    if (zero[i])
      return 3;
  base_tls += 2;
  if (base_step(3) != 5 || zero[3] != 1 || leaf_step() != 32 ||
      base_tls != 10 || leaf_tls != 22 || zero[3] != 2)
    return 4;
  ++main_tls;
  main_zero[32] = 0x7f;
  if (main_tls != 32 || main_zero[32] != 0x7f || base_address() != &base_tls ||
      zero != base_zero_address())
    return 5;
  const char output[] = {'D', 0, 'T'};
  return write(1, output, sizeof(output)) == sizeof(output) ? 83 : 6;
}
