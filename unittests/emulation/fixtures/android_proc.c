//===- android_proc.c - Independent live Android process callers ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned long U64;
typedef long I64;
extern int open(const char *, int, ...);
extern int close(int);
extern int dup(int);
extern I64 read(int, void *, U64);
extern I64 write(int, const void *, U64);
extern I64 lseek(int, I64, int);
extern I64 syscall(I64, ...);
extern int fstat(int, void *);
extern void *mmap(void *, U64, int, int, int, I64);
extern int mprotect(void *, U64, int);
extern int munmap(void *, U64);
extern void *opendir(const char *);
extern void *readdir(void *);
extern void rewinddir(void *);
extern int closedir(void *);
extern void *fopen(const char *, const char *);
extern char *fgets(char *, int, void *);
extern int fclose(void *);
extern int pthread_create(U64 *, const void *, void *(*)(void *), void *);
extern int pthread_join(U64, void **);
extern int pthread_mutex_lock(void *);
extern int pthread_mutex_unlock(void *);
extern int gettid(void);

static int equal(const char *A, const char *B) {
  while (*A && *A == *B) {
    ++A;
    ++B;
  }
  return *A == *B;
}
static int contains(const char *A, const char *B) {
  for (; *A; ++A) {
    const char *X = A, *Y = B;
    while (*Y && *X == *Y) {
      ++X;
      ++Y;
    }
    if (!*Y)
      return 1;
  }
  return 0;
}
static int task_count(void *D) {
  int Count = 0;
  for (;;) {
    const char *E = readdir(D);
    if (!E)
      return Count;
    if (E[19] >= '0' && E[19] <= '9')
      ++Count;
  }
}
static U64 Mutex;
static U64 ChildID;
static void *child(void *Cookie) {
  ChildID = gettid();
  void *S = fopen("/proc/thread-self/status", "r");
  if (!S)
    return (void *)91;
  char Line[160];
  int Found = 0;
  while (fgets(Line, sizeof(Line), S))
    if (equal(Line, "Pid:\t1001\n"))
      Found = 1;
  if (!Found || fclose(S))
    return (void *)92;
  pthread_mutex_lock(&Mutex);
  pthread_mutex_unlock(&Mutex);
  return Cookie;
}

U64 proc_thread_lifecycle(void) {
  U64 T;
  if (pthread_mutex_lock(&Mutex) || pthread_create(&T, 0, child, (void *)73))
    return 1;
  void *D = opendir("/proc/self/task");
  if (!D || task_count(D) != 2)
    return 2;
  void *S = fopen("/proc/self/status", "r");
  if (!S)
    return 3;
  int Found = 0;
  char Line[160];
  while (fgets(Line, sizeof(Line), S))
    if (equal(Line, "Threads:\t2\n"))
      Found = 1;
  if (!Found || fclose(S))
    return 4;
  if (pthread_mutex_unlock(&Mutex))
    return 5;
  void *Value;
  if (pthread_join(T, &Value) || Value != (void *)73 || ChildID != 1001)
    return 6;
  rewinddir(D);
  if (task_count(D) != 1 || closedir(D))
    return 7;
  if (open("/proc/1000/task/1001/status", 0) != -1)
    return 8;
  return 0;
}

U64 proc_live_maps(void) {
  char Buffer[4096];
  void *Region = mmap((void *)0x30000000, 8192, 3, 34, -1, 0);
  if (Region != (void *)0x30000000)
    return 1;
  int FD = open("/proc/self/maps", 0);
  if (FD < 0)
    return 2;
  I64 N = read(FD, Buffer, sizeof(Buffer) - 1);
  if (N < 0)
    return 3;
  Buffer[N] = 0;
  if (!contains(Buffer, "30000000-30002000 rw-p"))
    return 4;
  if (mprotect((char *)Region + 4096, 4096, 1) || lseek(FD, 0, 0))
    return 5;
  N = syscall(63, FD, Buffer, sizeof(Buffer) - 1, 0, 0, 0);
  if (N < 0)
    return 6;
  Buffer[N] = 0;
  if (!contains(Buffer, "30001000-30002000 r--p"))
    return 7;
  if (munmap(Region, 8192) || lseek(FD, 0, 0))
    return 8;
  N = read(FD, Buffer, sizeof(Buffer) - 1);
  if (N < 0)
    return 9;
  Buffer[N] = 0;
  if (contains(Buffer, "30000000-") || contains(Buffer, "30001000-"))
    return 10;
  return close(FD) ? 11 : 0;
}

U64 proc_write_stat(void) {
  int FD = open("/fixture/value", 2 | 524288);
  if (FD != 3)
    return 1;
  int Alias = dup(FD);
  if (Alias != 4 || lseek(Alias, 8192, 0) != 8192)
    return 2;
  if (write(FD, "XY", 2) != 2)
    return 3;
  U64 Stat[16];
  // Keep this OS fixture's independent checks scalar. Some AArch64 compilers
  // combine adjacent comparisons into SIMD even with vectorization disabled.
  const volatile U64 *Observed = Stat;
  if (fstat(Alias, Stat) || Observed[6] != 8194 || Observed[8] != 16 ||
      Observed[11] != 100 || Observed[12] != 7 || Observed[13] != 100 ||
      Observed[14] != 7)
    return 4;
  char Buffer[160];
  int Info = open("/proc/self/fdinfo/4", 0);
  I64 N = read(Info, Buffer, sizeof(Buffer) - 1);
  if (N < 0)
    return 5;
  Buffer[N] = 0;
  if (!equal(Buffer, "pos:\t8194\nflags:\t0400002\nmnt_id:\t1\nino:\t42\n"))
    return 6;
  if (close(Info) || close(FD) || close(Alias))
    return 7;
  return 0;
}
