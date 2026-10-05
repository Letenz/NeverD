/* Independently authored Android LP64 callers; no host libc or Android SDK. */
typedef unsigned long u64;
typedef __builtin_va_list va_list;
extern int sscanf(const char *, const char *, ...);
extern int vsscanf(const char *, const char *, va_list);
extern int *__errno(void);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);

__attribute__((noinline)) static int scan_va(const char *input,
                                             const char *format, ...) {
  va_list original, copy;
  __builtin_va_start(original, format);
  __builtin_va_copy(copy, original);
  int first = vsscanf(input, format, original);
  int second = vsscanf(input, format, copy);
  __builtin_va_end(copy);
  __builtin_va_end(original);
  return first == second ? first : -99;
}

__attribute__((noinline)) static int scan_stacked(const char *input,
                                                  const char *format, u64 a,
                                                  u64 b, u64 c, u64 d, u64 e,
                                                  u64 last, ...) {
  va_list args;
  __builtin_va_start(args, last);
  int result = vsscanf(input, format, args);
  __builtin_va_end(args);
  return a + b + c + d + e + last == 21 ? result : -99;
}

u64 scan_integers(unsigned char *out, u64 mode) {
  const char *input = " -128 65535 -2147483648 4294967295 -9223372036854775808 "
                      "18446744073709551615 -4294967298 100000002";
  const char *format = "%hhd %hu %d %u %lld %zu %td %jx%n";
#define OUTPUTS                                                                \
  (signed char *)out, (unsigned short *)(out + 16), (int *)(out + 32),         \
      (unsigned *)(out + 48), (long long *)(out + 64), (u64 *)(out + 80),      \
      (long *)(out + 96), (u64 *)(out + 112), (int *)(out + 128)
  *__errno() = 77;
  int result;
  if (mode == 0)
    result = sscanf(input, format, OUTPUTS);
  else if (mode == 1)
    result = scan_va(input, format, OUTPUTS);
  else
    result = scan_stacked(input, format, 1, 2, 3, 4, 5, 6, OUTPUTS);
#undef OUTPUTS
  return *__errno() == 77 ? (unsigned)result : 0x100000000UL;
}

u64 scan_supplied(unsigned *out, const char *input, const char *format,
                  u64 mode) {
  *__errno() = 77;
  int result = mode ? scan_va(input, format, out, out + 4, out + 8, out + 12)
                    : sscanf(input, format, out, out + 4, out + 8, out + 12);
  return *__errno() == 77 ? (unsigned)result : 0x100000000UL;
}

u64 scan_explicit_va(const char *input, const char *format, void *list) {
  return (unsigned)vsscanf(input, format, *(va_list *)list);
}

u64 scan_alias(unsigned *out) {
  return (unsigned)sscanf("12,34", "%u,%u", out, out);
}

u64 scan_dynamic(unsigned *out, u64 closed, u64 variadic) {
  void *library = dlopen("libscan-model.so", 2);
  void *entry = dlsym(library, variadic ? "vsscanf" : "sscanf");
  if (!entry || (closed && dlclose(library)))
    return 999;
  if (variadic) {
    /* The explicit va_list path is tested separately; dynamic resolution also
       records the provider for the non-variadic public entry point. */
    struct {
      void *stack, *gr_top, *vr_top;
      int gr_offs, vr_offs;
    } list = {&out, 0, 0, 0, 0};
    return (unsigned)((int (*)(const char *, const char *, va_list))entry)(
        "017", "%o", *(va_list *)&list);
  }
  return (unsigned)((int (*)(const char *, const char *, ...))entry)("017",
                                                                     "%o", out);
}
