// Independent API 28 LP64 guest-thread workloads. No Android headers or host
// threading implementation is linked into these fixtures.
typedef unsigned long u64;
typedef unsigned int u32;
typedef struct {
  u32 flags, padding;
  u64 stack, size, guard;
  int policy, priority;
  unsigned char reserved[16];
} Attr;
extern int pthread_create(u64 *, const Attr *, void *(*)(void *), void *);
extern int pthread_join(u64, void **);
extern int pthread_detach(u64);
extern u64 pthread_self(void);
extern int pthread_equal(u64, u64);
extern int pthread_getattr_np(u64, Attr *);
extern int pthread_gettid_np(u64);
extern void pthread_exit(void *) __attribute__((noreturn));
extern int pthread_attr_init(Attr *);
extern int pthread_attr_setdetachstate(Attr *, int);
extern int pthread_attr_setstacksize(Attr *, u64);
extern int pthread_attr_setstack(Attr *, void *, u64);
extern int pthread_attr_setguardsize(Attr *, u64);
extern int pthread_once(int *, void (*)(void));
extern int pthread_mutex_init(void *, const void *);
extern int pthread_mutex_lock(void *);
extern int pthread_mutex_unlock(void *);
extern int *__errno(void);
extern int gettid(void);
extern int getpid(void);
extern long syscall(long, ...);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);
extern char *dlerror(void);
extern int __cxa_atexit(void (*)(void *), void *, void *);
extern void __cxa_finalize(void *);

static u64 *Shared;
static u64 tls(void) {
  u64 value;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(value));
  return value;
}
static u64 raw_call(u64 number, u64 arg) {
  register u64 x8 __asm__("x8") = number;
  register u64 x0 __asm__("x0") = arg;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory", "cc");
  return x0;
}
static void inner_once(void) { ++Shared[(gettid() - 1000) * 32 + 14]; }
static void outer_once(void) {
  int inner = 0;
  pthread_once(&inner, inner_once);
  ++Shared[(gettid() - 1000) * 32 + 14];
}
static void finalize(void *arg) {
  u64 *row = arg;
  row[15] = (u64)gettid();
}
static void register_window(u64 *row, u64 identity) {
  u64 seed = 0x0123456789abcdefUL + identity;
  u64 rounding = identity << 22, status = identity;
  // Branches do not change NZCV. Each quantum must preserve the full vector,
  // scalar loop counter, condition flags and FP controls independently.
  __asm__ volatile("dup v16.2d, %1\n"
                   "msr fpcr, %2\nmsr fpsr, %3\ncmp %3, #1\n"
                   "mov x9, #113\n1: sub x9, x9, #1\ncbnz x9, 1b\n"
                   "str q16, [%0]\nmrs x10, fpcr\nstr x10, [%0, #16]\n"
                   "mrs x10, fpsr\nstr x10, [%0, #24]\n"
                   "cset x10, eq\nstr x10, [%0, #32]\n"
                   "msr fpcr, xzr\nmsr fpsr, xzr"
                   :
                   : "r"(row + 24), "r"(seed), "r"(rounding), "r"(status)
                   : "x9", "x10", "v16", "memory", "cc");
}
static void *worker(void *arg) {
  u64 *row = arg;
  const u64 identity = row[31];
  volatile u64 local = 0xfeed0000 + identity;
  row[0] = pthread_self();
  row[1] = gettid();
  row[2] = raw_call(178, 0);
  row[3] = syscall(178);
  row[4] = getpid();
  row[5] = tls();
  row[6] = (u64)__errno();
  row[7] = *__errno();
  row[9] = (u64)&local;
  row[10] = ((u64 *)tls())[1];
  row[12] = (u64)dlerror();
  *__errno() = 100 + identity;
  dlopen("missing-child.so", 0);
  while (!__atomic_load_n(Shared, __ATOMIC_ACQUIRE)) {
  }
  register_window(row, identity);
  row[11] = (u64)dlerror();
  row[8] = *__errno();
  row[16] = (u64)dlerror();
  u64 mutex[5] = {0x4000, 0, 0, 0, 0};
  pthread_mutex_lock(mutex);
  row[13] = ((u32 *)mutex)[1];
  pthread_mutex_unlock(mutex);
  int once = 0;
  pthread_once(&once, outer_once);
  pthread_once(&once, outer_once);
  __cxa_atexit(finalize, row, row);
  __cxa_finalize(row);
  Attr attr;
  pthread_getattr_np(pthread_self(), &attr);
  row[20] = attr.stack;
  row[21] = attr.size;
  row[22] = attr.guard;
  row[23] = local;
  return (void *)(0xfedc000000000000UL + identity);
}
u64 threads_identity(u64 *out) {
  Shared = out;
  out[1] = (u64)__errno();
  out[2] = tls();
  out[5] = gettid();
  out[11] = pthread_self();
  *__errno() = 77;
  dlopen("missing-parent.so", 0);
  out[32 + 31] = 1;
  out[64 + 31] = 2;
  if (pthread_create(out + 6, 0, worker, out + 32))
    return 1;
  if (pthread_create(out + 7, 0, worker, out + 64))
    return 2;
  out[12] = pthread_gettid_np(out[6]);
  __atomic_store_n(out, 1, __ATOMIC_RELEASE);
  out[10] = pthread_join(out[6], (void **)(out + 8));
  out[10] |= pthread_join(out[7], (void **)(out + 9));
  out[3] = *__errno();
  out[4] = (u64)dlerror();
  out[13] = pthread_equal(pthread_self(), out[11]);
  return 0;
}
static void *short_worker(void *arg) {
  u64 *out = arg;
  out[2] = gettid();
  return (void *)0x1234567800000042UL;
}
u64 threads_detached(u64 *out, u64 mode) {
  Attr attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, 1);
  out[0] = pthread_create(out + 1, mode ? 0 : &attr, short_worker, out);
  if (out[0])
    return out[0];
  if (mode)
    out[3] = pthread_detach(out[1]);
  out[4] = pthread_join(out[1], 0);
  out[5] = pthread_detach(out[1]);
  return 73;
}
static void *exit_worker(void *arg) {
  u64 mode = (u64)arg;
  if (mode == 0)
    pthread_exit((void *)0xfedcba9876543210UL);
  if (mode == 1)
    syscall(93, 7);
  if (mode == 2)
    raw_call(93, 9);
  if (mode == 3)
    raw_call(94, 37);
  if (mode == 4)
    syscall(94, 38);
  return (void *)1;
}
u64 threads_exit(u64 *out, u64 mode) {
  out[0] = pthread_create(out + 1, 0, exit_worker, (void *)mode);
  if (out[0])
    return out[0];
  out[2] = pthread_join(out[1], (void **)(out + 3));
  out[4] = 1;
  return 74;
}
u64 threads_limits(u64 *out, u64 mode) {
  Attr attr;
  pthread_attr_init(&attr);
  if (mode == 1)
    pthread_attr_setstacksize(&attr, ~0UL);
  if (mode == 2)
    pthread_attr_setstack(&attr, (void *)0x20000000, 16384);
  if (mode == 3)
    attr.policy = 2;
  if (mode == 4)
    pthread_attr_setguardsize(&attr, ~0UL);
  if (mode == 5)
    attr.flags |= 0x100;
  out[1] = 0x12345678;
  *__errno() = 91;
  out[0] = pthread_create(out + 1, &attr, short_worker, out);
  if (mode == 0 && !out[0]) {
    out[3] = pthread_join(out[1], 0);
    out[4] = pthread_create(out + 5, 0, short_worker, out);
  }
  out[6] = *__errno();
  return 0;
}
u64 threads_invalid(u64 *out, u64 mode) {
  if (mode == 0)
    return pthread_join(pthread_self(), 0);
  if (mode == 1)
    return pthread_join(0, 0);
  if (mode == 2)
    return pthread_getattr_np(1, (Attr *)out);
  if (mode == 3)
    return pthread_create((u64 *)1, 0, short_worker, out);
  if (mode == 4)
    return pthread_create(out, 0, (void *(*)(void *))1, out);
  if (mode == 5)
    return pthread_create(out, 0, (void *(*)(void *))out, out);
  return pthread_create(out, (Attr *)1, short_worker, out);
}
u64 threads_dynamic(u64 *out, u64 closed) {
  void *lib = dlopen("libthread-model.so", 0);
  int (*create)(u64 *, const Attr *, void *(*)(void *), void *) =
      dlsym(lib, "pthread_create");
  int (*join)(u64, void **) = dlsym(lib, "pthread_join");
  if (closed)
    dlclose(lib);
  out[0] = create(out + 1, 0, short_worker, out);
  if (!out[0])
    out[3] = join(out[1], (void **)(out + 4));
  return 75;
}
static void *join_peer(void *arg) {
  u64 *out = arg;
  while (!__atomic_load_n(out + 1, __ATOMIC_ACQUIRE)) {
  }
  out[2] = pthread_join(out[0], 0);
  return 0;
}
u64 threads_join_cycle(u64 *out) {
  out[0] = pthread_self();
  u64 child;
  pthread_create(&child, 0, join_peer, out);
  __atomic_store_n(out + 1, 1, __ATOMIC_RELEASE);
  return pthread_join(child, 0);
}
u64 threads_join_finished(u64 *out, u64 detach) {
  pthread_create(out + 1, 0, short_worker, out);
  // Enough guest work lets the child return before the parent's query.
  for (volatile unsigned i = 0; i < 400; ++i) {
  }
  out[3] = pthread_gettid_np(out[1]);
  return detach ? pthread_detach(out[1])
                : pthread_join(out[1], (void **)(out + 4));
}
static void *guard_worker(void *arg) {
  Attr attr;
  pthread_getattr_np(pthread_self(), &attr);
  *(volatile unsigned char *)attr.stack = 1;
  return arg;
}
static void exiting_once(void) { pthread_exit((void *)9); }
static void *callback_exit_worker(void *arg) {
  int control = 0;
  pthread_once(&control, exiting_once);
  return arg;
}
static void *bad_tls_worker(void *arg) {
  ((volatile u64 *)tls())[1] = 1;
  return (void *)pthread_self();
}
u64 threads_boundary(u64 *out, u64 mode) {
  void *(*start)(void *) = mode == 0   ? guard_worker
                           : mode == 1 ? callback_exit_worker
                                       : bad_tls_worker;
  pthread_create(out + 1, 0, start, out);
  return pthread_join(out[1], 0);
}
u64 threads_last_exit(u64 *out, u64 mode) {
  (void)out;
  if (mode)
    syscall(93, 263);
  else
    raw_call(93, 264);
  return 1;
}
