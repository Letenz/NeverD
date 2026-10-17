/* Independently authored callers; expected values come from the pinned BSD
 * implementation, never the host libc's random generator. */
typedef unsigned long u64;
typedef unsigned int u32;
extern int rand(void);
extern long random(void);
extern void srand(unsigned);
extern void srandom(unsigned);
extern int rand_r(unsigned *);
extern int *__errno(void);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);
extern int pthread_create(u64 *, const void *, void *(*)(void *), void *);
extern int pthread_join(u64, void **);
extern int mprotect(void *, u64, int);
extern char *initstate(unsigned, char *, u64);

u64 random_sequence(u32 *out, u64 seed, u64 mode) {
  *__errno() = 77;
  if (mode == 1)
    srand(seed);
  if (mode == 2)
    srandom(seed);
  for (unsigned i = 0; i != 64; ++i)
    out[i] = i & 1 ? random() : rand();
  return *__errno() != 77;
}
u64 random_reentrant(u32 *out, unsigned *seed) {
  *__errno() = 91;
  for (unsigned i = 0; i != 8; ++i) {
    out[2 * i] = rand_r(seed);
    out[2 * i + 1] = *seed;
  }
  out[16] = rand();
  return *__errno() != 91;
}
u64 random_readonly(unsigned *seed) {
  if (mprotect((void *)((u64)seed & ~4095UL), 4096, 1))
    return 1;
  return rand_r(seed);
}
u64 random_dynamic(u32 *out, u64 stale) {
  void *lib = dlopen("libprng.so", 2);
  if (!lib)
    return 1;
  void (*seed)(unsigned) = dlsym(lib, "srandom");
  int (*next)(void) = dlsym(lib, "rand");
  if (!seed || !next)
    return 2;
  seed(42);
  out[0] = next();
  out[1] = random();
  if (dlclose(lib))
    return 3;
  out[2] = stale ? next() : rand();
  return 0;
}
static void *random_worker(void *out) {
  u32 *p = out;
  for (unsigned i = 0; i != 32; ++i)
    p[i] = rand();
  return 0;
}
u64 random_threads(u32 *out) {
  u64 thread;
  srand(42);
  if (pthread_create(&thread, 0, random_worker, out + 32))
    return 1;
  for (unsigned i = 0; i != 32; ++i)
    out[i] = random();
  return pthread_join(thread, 0);
}
u64 random_reseed(u32 *out) {
  srand(42);
  out[0] = rand();
  out[1] = random();
  srandom(1);
  out[2] = rand();
  out[3] = random();
  srand(42);
  out[4] = random();
  return 0;
}
u64 random_custom_state(char *out) { return (u64)initstate(1, out, 128); }
