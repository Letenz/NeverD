// Original dylib import-addend and resident-alias workload.
extern int provider_values[4];
extern int provider_probe(void);
extern int *from_tail;
int *at_third = provider_values + 2;
__thread int *volatile tls_at_third = provider_values + 2;
int main(void) {
  if (provider_probe() != 23)
    return 3;
  if (at_third != from_tail || *at_third != 17)
    return 1;
  if (tls_at_third != at_third || *tls_at_third != 17)
    return 4;
  int *saved_tls = tls_at_third;
  tls_at_third = provider_values + 1;
  if (*tls_at_third != 13 || *saved_tls != 17)
    return 5;
  tls_at_third = saved_tls;
  *tls_at_third = 23;
  if (*from_tail != 23)
    return 6;
  *from_tail = 29;
  if (provider_values[2] != 29 || *at_third != 29)
    return 2;
  tls_at_third = provider_values + 1;
  return provider_values[0] + provider_values[1] + provider_values[2] +
         provider_values[3] + 6;
}
