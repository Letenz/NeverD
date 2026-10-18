//===- android_copy.c - Independent bounded-copy callers ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Freestanding API 28 callers for destination bytes and pointer boundaries.

typedef unsigned long u64;
extern char *strncpy(char *, const char *, u64);
extern int *__errno(void);
extern int mprotect(void *, u64, int);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);

u64 copy_raw(char *dst, const char *src, u64 count) {
  return (u64)strncpy(dst, src, count);
}
u64 copy_supplied(char *dst, const char *src, u64 count, unsigned *error) {
  *__errno() = 733;
  char *result = strncpy(dst, src, count);
  *error = *__errno();
  return (u64)result;
}
u64 copy_readonly(void *page, char *dst, const char *src, u64 count) {
  if (mprotect(page, 4096, 1))
    return 98;
  return (u64)strncpy(dst, src, count);
}
u64 copy_dynamic(char *dst, u64 closed) {
  void *library = dlopen("libcopy.so", 2);
  if (!library)
    return 97;
  char *(*copy)(char *, const char *, u64) = dlsym(library, "strncpy");
  if (!copy)
    return 96;
  if (closed && dlclose(library))
    return 95;
  char *result = copy(dst, "ab", 6);
  if (dlclose(library))
    return 94;
  return (u64)result;
}
