/* Independently authored API 28 AArch64 syscall wrapper workloads. */
typedef unsigned long u64;
extern long syscall(long, ...);
extern int *__errno(void);
extern int getpid(void);
extern int gettid(void);
extern unsigned int getuid(void);
extern unsigned int geteuid(void);
extern unsigned int getgid(void);
extern unsigned int getegid(void);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);

static long raw(u64 number, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f) {
  register u64 x0 __asm__("x0") = a;
  register u64 x1 __asm__("x1") = b;
  register u64 x2 __asm__("x2") = c;
  register u64 x3 __asm__("x3") = d;
  register u64 x4 __asm__("x4") = e;
  register u64 x5 __asm__("x5") = f;
  register u64 x8 __asm__("x8") = number;
  __asm__ volatile("svc #0"
                   : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                   : "memory");
  return (long)x0;
}

u64 syscall_identities(u64 *out) {
  *__errno() = 77;
  const u64 named[] = {getpid(),  gettid(), getuid(),
                       geteuid(), getgid(), getegid()};
  const long numbers[] = {172, 178, 174, 175, 176, 177};
  for (unsigned i = 0; i < 6; ++i) {
    out[i] = syscall(numbers[i]);
    if (out[i] != named[i] || out[i] != (u64)raw(numbers[i], 0, 0, 0, 0, 0, 0))
      return 1;
  }
  return *__errno() == 77 ? 0 : 2;
}

u64 syscall_errors(u64 *out) {
  *__errno() = 77;
  out[0] = raw(64, 99, 0, 1, 0, 0, 0);
  out[1] = *__errno();
  out[2] = syscall(64, 99UL, 0UL, 1UL);
  out[3] = *__errno();
  out[4] = syscall(64, 1UL, 0UL, 1UL);
  out[5] = *__errno();
  out[6] = syscall(64, 1UL, 0UL, 0UL);
  out[7] = *__errno();
  return 0;
}

u64 syscall_memory(u64 *out) {
  *__errno() = 77;
  /* The sixth argument is the offset. An extra x7 value must be ignored. */
  long invalid = syscall(222, 0UL, 4096UL, 3UL, 0x22UL, ~0UL, 1UL, 0UL);
  if (invalid != -1 || *__errno() != 22)
    return 1;
  *__errno() = 77;
  unsigned char *page =
      (void *)syscall(222, 0x180000000UL, 4096UL, 3UL, 0x22UL, ~0UL, 0UL, 1UL);
  if ((long)page == -1 || *__errno() != 77)
    return 2;
  out[0] = (u64)page;
  page[0] = 'M';
  page[4095] = 0xa5;
  if (syscall(226, page, 4096UL, 1UL) || page[4095] != 0xa5 ||
      syscall(64, 1UL, page, 1UL) != 1 || syscall(226, page, 4096UL, 3UL))
    return 3;
  page[0] = 'N';
  out[1] = page[0];
  if (syscall(215, page, 4096UL))
    return 4;
  out[2] = *__errno();
  out[3] = syscall(64, 1UL, page, 1UL);
  out[4] = *__errno();
  return 0;
}

u64 syscall_output(void) {
  const unsigned char text[] = {'A', 0, 0xff, '\n'};
  const struct {
    const void *base;
    u64 size;
  } vectors[] = {{text, 1}, {text + 1, 3}};
  *__errno() = 77;
  if (syscall(66, 2UL, vectors, 2UL) != 4 || *__errno() != 77)
    return 1;
  return 0;
}

u64 syscall_invoke(long number, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f,
                   u64 ignored) {
  return syscall(number, a, b, c, d, e, f, ignored);
}

u64 syscall_dynamic(u64 after_close) {
  typedef long (*syscall_fn)(long, ...);
  *__errno() = 77;
  void *handle = dlopen("libservice.so", 2);
  if (!handle)
    return 1;
  syscall_fn call = (syscall_fn)dlsym(handle, "syscall");
  if (!call)
    return 2;
  if (after_close && dlclose(handle))
    return 3;
  long result = call(178);
  if (!after_close && dlclose(handle))
    return 3;
  return result == gettid() && *__errno() == 77 ? 0 : 4;
}

/* Raw and Bionic paths retain their different error encodings. */
u64 syscall_unavailable_kernel(u64 *out) {
  *__errno() = 77;
  out[0] = (u64)raw(434, 1000, 0, 0, 0, 0, 0);
  out[1] = (u64)*__errno();
  out[2] = (u64)syscall(434, 1000UL, 0UL);
  out[3] = (u64)*__errno();
  out[4] = (u64)syscall(434, ~0UL, ~0UL);
  out[5] = (u64)*__errno();
  return 0;
}
