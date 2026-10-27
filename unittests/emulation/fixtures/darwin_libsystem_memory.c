//===- darwin_libsystem_memory.c - Ordinary memory mapping calls ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Independently authored, SDK-free C workload. No host implementation copied.
extern int *__error(void);
extern void *mmap(void *, unsigned long, int, int, int, long long);
extern int mprotect(void *, unsigned long, int);
extern int munmap(void *, unsigned long);
extern int open(const char *, int, ...);
extern int close(int);
extern long pread(int, void *, unsigned long, long long);
extern long write(int, const void *, unsigned long);

int main(int argc, char **argv) {
  if (argc != 3)
    return 1;
  unsigned long page = 0;
  for (const char *p = argv[1]; *p; ++p)
    page = page * 10 + (unsigned)(*p - '0');
  if (page != 4096 && page != 16384)
    return 2;
  int *error = __error();
  *error = 71;
  // The C wrapper rejects these before unsupported protection/FD handling.
  if (mmap((void *)1, 0, 0x7fffffff, 0x1002, 31, 1) != (void *)-1 ||
      *error != 22)
    return 3;
  if (mmap((void *)0, page, 0x7fffffff, 0x1000, -1, 0) != (void *)-1 ||
      *error != 22)
    return 4;
  unsigned char *a = mmap((void *)0, page, 3, 0x1002, -1, 0);
  if (a == (void *)-1 || *error != 22)
    return 5;
  const unsigned long long *words = (const unsigned long long *)a;
  for (unsigned long i = 0; i != page / 8; ++i)
    if (words[i] != 0)
      return 6;
  a[0] = 'M';
  a[page - 1] = 0x80;
  if (mprotect(a, page, 1) || *error != 22 || a[page - 1] != 0x80)
    return 7;
  if (mprotect(a + 1, page, 1) != -1 || *error != 22 || a[0] != 'M')
    return 8;
  if (mprotect(a, page, 3) || *error != 22)
    return 9;
  a[1] = 0;
  a[2] = 'P';
  if (write(1, a, 3) != 3 || munmap(a, page) || *error != 22)
    return 10;
  // UNIX03 offset alignment precedes descriptor validation.
  if (mmap((void *)0, page, 3, 2, -1, 1) != (void *)-1 || *error != 22)
    return 11;
  if (mmap((void *)0, page, 3, 2, -1, 0) != (void *)-1 || *error != 9)
    return 12;
  int fd = open(argv[2], 0);
  if (fd < 0 || *error != 9)
    return 13;
  unsigned char *f = mmap((void *)0, page, 3, 2, fd, (long long)page);
  if (f == (void *)-1 || close(fd) || *error != 9)
    return 14;
  if (f[0] != 'f' || f[1] || f[2] != 'l')
    return 15;
  for (unsigned long i = 3; i != 8; ++i)
    if (f[i] != 0)
      return 16;
  words = (const unsigned long long *)f;
  for (unsigned long i = 1; i != page / 8; ++i)
    if (words[i] != 0)
      return 16;
  f[0] = 'X';
  f[page - 1] = 'Z';
  fd = open(argv[2], 0);
  unsigned char original[3] = {0, 0, 0};
  if (fd < 0 || pread(fd, original, 3, (long long)page) != 3 ||
      original[0] != 'f' || original[1] || original[2] != 'l' || close(fd) ||
      munmap(f, page) || *error != 9)
    return 17;
  return 79;
}
