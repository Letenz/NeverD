//===- linux_files.c - Original Linux and Android file callers -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned long U64;
typedef long I64;
typedef unsigned int U32;
#if defined(__aarch64__)
static const U64 UserLimit = 0x0001000000000000UL;
enum {
  OpenAt = 56,
  Read = 63,
  Close = 57,
  Seek = 62,
  Fstat = 80,
  Write = 64,
  Protect = 226,
  Exit = 93,
  LargeFile = 0400000
};
#else
static const U64 UserLimit = 0x0000800000000000UL;
enum {
  OpenAt = 257,
  Read = 0,
  Close = 3,
  Seek = 8,
  Fstat = 5,
  Write = 1,
  Protect = 10,
  Exit = 60,
  LargeFile = 00100000
};
#endif

static U64 raw(U64 Number, U64 A, U64 B, U64 C, U64 D) {
#if defined(__aarch64__)
  register U64 N __asm__("x8") = Number;
  register U64 X0 __asm__("x0") = A;
  register U64 X1 __asm__("x1") = B;
  register U64 X2 __asm__("x2") = C;
  register U64 X3 __asm__("x3") = D;
  __asm__ volatile("svc #0"
                   : "+r"(X0)
                   : "r"(N), "r"(X1), "r"(X2), "r"(X3)
                   : "memory", "cc");
  return X0;
#else
  register U64 R10 __asm__("r10") = D;
  U64 Result;
  __asm__ volatile("syscall"
                   : "=a"(Result)
                   : "a"(Number), "D"(A), "S"(B), "d"(C), "r"(R10)
                   : "memory", "cc", "rcx", "r11");
  return Result;
#endif
}

static unsigned char Pages[8192] __attribute__((aligned(4096)));
static const char Path[] = "/fixture/data";
static const unsigned char Expected[] = {0, 0xff, 0x41, 0x0a, 0x80, 0x5a};
#define CHECK(A, B)                                                            \
  do {                                                                         \
    if ((U64)(A) != (U64)(B))                                                  \
      return __LINE__;                                                         \
  } while (0)

U64 files_sequence(void) {
  U64 F = raw(OpenAt, 0x77889900ffffffffUL, (U64)Path,
              0x1122334400000000UL | LargeFile | 02000000, 0777);
  CHECK(F, 3);
  U64 Other = raw(OpenAt, (U64)-100, (U64)Path, 0, 0);
  CHECK(Other, 4);
  CHECK(raw(Read, F | 0x1234567800000000UL, (U64)Pages, 2, 0), 2);
  CHECK(raw(Read, F, (U64)Pages + 2, 100, 0), 4);
  for (unsigned I = 0; I < sizeof(Expected); ++I)
    CHECK(((volatile unsigned char *)Pages)[I], Expected[I]);
  CHECK(raw(Read, F, 1, 2, 0), 0); // EOF does not probe payload mappings.
  CHECK(raw(Read, Other, (U64)Pages + 8, 1, 0), 1);
  CHECK(Pages[8], 0);
  CHECK(raw(Seek, F, (U64)-2, 0x1234567800000002UL, 0), 4);
  CHECK(raw(Read, F, (U64)Pages + 10, 2, 0), 2);
  CHECK(Pages[10], 0x80);
  CHECK(Pages[11], 0x5a);
  CHECK(raw(Seek, F, (U64)-7, 0, 0), (U64)-22);
  CHECK(raw(Seek, F, 0, 1, 0), 6);
  CHECK(raw(Seek, F, 0x7fffffffffffffffUL, 0, 0), 0x7fffffffffffffffUL);
  CHECK(raw(Seek, F, 1, 1, 0), (U64)-22);
  CHECK(raw(Read, F, (U64)Pages, 1, 0), (U64)-22);
  CHECK(raw(Seek, F, 0, 1, 0), 0x7fffffffffffffffUL);
  CHECK(raw(Read, F, UserLimit, 0, 0), 0);
  CHECK(raw(Seek, F, (U64)-1, 1, 0), 0x7ffffffffffffffeUL);
  CHECK(raw(Read, F, 1, 1, 0), 0);
  CHECK(raw(Read, F, 1, 2, 0), (U64)-22);
  CHECK(raw(Seek, F, 0, 1, 0), 0x7ffffffffffffffeUL);
  // Validate the original count before transfer clamping or EOF.
  CHECK(raw(Seek, F, 0x7fffffff7fffffffUL, 0, 0), 0x7fffffff7fffffffUL);
  CHECK(raw(Read, F, 1, 0x100000000UL, 0), (U64)-22);
  CHECK(raw(Read, F, (U64)-1, 0x100000000UL, 0), (U64)-14);
  CHECK(raw(Seek, F, 0, 1, 0), 0x7fffffff7fffffffUL);
  CHECK(raw(Write, F, 1, 1, 0), (U64)-9);
  CHECK(raw(Close, F | 0x1234567800000000UL, 0, 0, 0), 0);
  CHECK(raw(Close, F, 0, 0, 0), (U64)-9);
  CHECK(raw(Read, F, (U64)-1, (U64)-1, 0), (U64)-9);
  CHECK(raw(OpenAt, (U64)-100, (U64)Path, 0, 0), 3);
  CHECK(raw(Close, 1, 0, 0, 0), 0);
  CHECK(raw(Write, 1, 1, 0, 0), (U64)-9);
  CHECK(raw(OpenAt, (U64)-100, (U64)Path, 0, 0), 1);
  CHECK(raw(Write, 1, (U64)Path, 1, 0), (U64)-9);
  CHECK(raw(Read, 1, (U64)Pages + 12, 3, 0), 3);
  CHECK(Pages[14], 0x41);
  CHECK(raw(Close, 0, 0, 0, 0), 0);
  CHECK(raw(OpenAt, (U64)-100, (U64)Path, 0, 0), 0);
#if defined(__x86_64__)
  CHECK(raw(2, (U64)Path, 0, 0, 0), 5);
#endif
  return 0;
}

U64 files_faults(void) {
  CHECK(raw(OpenAt, 0, (U64) "", 0, 0), (U64)-2);
  CHECK(raw(OpenAt, 0, 1, 0, 0), (U64)-14);
  CHECK(raw(OpenAt, 0, (U64) "/fixture/missing", 0, 0), (U64)-2);
  CHECK(raw(OpenAt, 0, (U64) "/fixture/data/child", 0, 0), (U64)-20);
  U64 F = raw(OpenAt, 0, (U64)Path, 0, 0);
  CHECK(F, 3);
  CHECK(raw(Read, F, 1, 0, 0), 0);
  CHECK(raw(Read, F, UserLimit, 0, 0), 0);
  CHECK(raw(Read, F, UserLimit - 1, 0, 0), 0);
  CHECK(raw(Read, F, UserLimit + 1, 0, 0), (U64)-14);
  CHECK(raw(Read, F, UserLimit, 1, 0), (U64)-14);
  CHECK(raw(Read, F, (U64)-1, 0, 0), (U64)-14);
  CHECK(raw(Read, F, (U64)Pages, (U64)-1, 0), (U64)-14);
  CHECK(raw(Read, 1, (U64)-1, 0, 0), (U64)-9);
  CHECK(raw(Seek, F, 0, 99, 0), (U64)-22);
  CHECK(raw(Seek, 1, 0, 0, 0), (U64)-29);
  CHECK(raw(Protect, (U64)Pages + 4096, 4096, 1, 0), 0);
  CHECK(raw(Read, F, (U64)Pages + 4094, 6, 0), 2);
  CHECK(Pages[4094], 0);
  CHECK(Pages[4095], 0xff);
  CHECK(raw(Read, F, (U64)Pages + 4096, 2, 0), (U64)-14);
  CHECK(raw(Seek, F, 0, 1, 0), 2);
  CHECK(raw(Read, F, (U64)Pages, 6, 0), 4);
  CHECK(Pages[0], 0x41);
  CHECK(Pages[3], 0x5a);
  CHECK(raw(Read, F, (U64)Pages + 4096, 1, 0), 0);
  // A terminator at the last readable byte must not probe the next page.
  CHECK(raw(Protect, (U64)Pages + 4096, 4096, 0, 0), 0);
  for (unsigned I = 0; I < sizeof(Path); ++I)
    Pages[4096 - sizeof(Path) + I] = Path[I];
  CHECK(raw(OpenAt, 0, (U64)Pages + 4096 - sizeof(Path), 0, 0), 4);
  Pages[4095] = 'x';
  CHECK(raw(OpenAt, 0, (U64)Pages + 4096 - sizeof(Path), 0, 0), (U64)-14);
  return 0;
}

U64 files_capacity(void) {
  CHECK(raw(OpenAt, 0, (U64)Path, 0, 0), 3);
  CHECK(raw(OpenAt, 0, (U64) "/absent", 0, 0), (U64)-24);
  CHECK(raw(OpenAt, 0, 1, 0, 0), (U64)-14);
  CHECK(raw(Close, 3, 0, 0, 0), 0);
  CHECK(raw(OpenAt, 0, (U64) "/absent", 0, 0), (U64)-2);
  CHECK(raw(OpenAt, 0, (U64)Path, 0, 0), 3);
  return 0;
}

struct FileTime {
  I64 Seconds;
  U64 Nanoseconds;
};
struct FileStatus {
  U64 Device, Inode;
#if defined(__aarch64__)
  U32 Mode, Links, UID, GID;
  U64 RDevice, Pad1;
  I64 Size;
  int BlockSize, Pad2;
#else
  U64 Links;
  U32 Mode, UID, GID, Pad1;
  U64 RDevice;
  I64 Size, BlockSize;
#endif
  I64 Blocks;
  struct FileTime Access, Modification, Change;
#if defined(__aarch64__)
  U32 Reserved[2];
#else
  U64 Reserved[3];
#endif
};
#if defined(__aarch64__)
_Static_assert(sizeof(struct FileStatus) == 128, "AArch64 stat ABI");
#else
_Static_assert(sizeof(struct FileStatus) == 144, "x64 stat ABI");
#endif
_Static_assert(__builtin_offsetof(struct FileStatus, BlockSize) == 56,
               "stat block-size offset");

// Keep independent field observations scalar under both optimization levels.
static U64 verify_status(const volatile struct FileStatus *S) {
  CHECK(S->Device, 0xfe12cd34UL);
  CHECK(S->Inode, 0xfedcba9876543210UL);
  CHECK(S->Mode, 0100644);
  CHECK(S->Links, 0x89abcdefUL);
  CHECK(S->UID, 0x87654321UL);
  CHECK(S->GID, 0xfedcba98UL);
  CHECK(S->RDevice, 0);
  CHECK(S->Pad1, 0);
#if defined(__aarch64__)
  CHECK(S->Pad2, 0);
#endif
  CHECK(S->Size, 0); // Explicit metadata is independent of readable bytes.
  CHECK(S->BlockSize, 16384);
  CHECK(S->Blocks, 0x1234567890UL);
  CHECK(S->Access.Seconds, -0x7fffffffffffffffL);
  CHECK(S->Access.Nanoseconds, 123456789);
  CHECK(S->Modification.Seconds, 4294967297L);
  CHECK(S->Modification.Nanoseconds, 987654321);
  CHECK(S->Change.Seconds, 0x7fffffffffffffffL);
  CHECK(S->Change.Nanoseconds, 999999999);
  for (unsigned I = 0; I < sizeof(S->Reserved) / sizeof(S->Reserved[0]); ++I)
    CHECK(S->Reserved[I], 0);
  return 0;
}

U64 files_status(void) {
  struct {
    U64 Before;
    struct FileStatus Status;
    U64 After;
  } Out;
  volatile unsigned char *Bytes = (volatile unsigned char *)&Out;
  for (unsigned I = 0; I < sizeof(Out); ++I)
    Bytes[I] = 0xa5;
  U64 F = raw(OpenAt, 0, (U64)Path, 0, 0);
  CHECK(F, 3);
  CHECK(raw(Read, F, (U64)Pages, 1, 0), 1);
  CHECK(raw(Fstat, F | 0xabcdef0000000000UL, (U64)&Out.Status, 0, 0), 0);
  CHECK(verify_status(&Out.Status), 0);
  CHECK(Out.Before, 0xa5a5a5a5a5a5a5a5UL);
  CHECK(Out.After, 0xa5a5a5a5a5a5a5a5UL);
  CHECK(raw(Seek, F, 0, 1, 0), 1);
  CHECK(raw(Read, F, (U64)Pages, 1, 0), 1);
  CHECK(Pages[0], 0xff);
  CHECK(raw(Fstat, F, (U64)Pages + 1, 0, 0), 0);
  for (unsigned I = 0; I < sizeof(Out.Status); ++I)
    CHECK(((volatile unsigned char *)Pages)[I + 1],
          ((volatile unsigned char *)&Out.Status)[I]);
  CHECK(raw(Fstat, F, 1, 0, 0), (U64)-14);
  CHECK(raw(Fstat, F, UserLimit - sizeof(Out.Status) + 1, 0, 0), (U64)-14);
  CHECK(raw(Protect, (U64)Pages + 4096, 4096, 1, 0), 0);
  CHECK(raw(Fstat, F, (U64)Pages + 4096, 0, 0), (U64)-14);
  CHECK(raw(Seek, F, 0, 1, 0), 2);
  CHECK(raw(Close, F, 0, 0, 0), 0);
  CHECK(raw(Fstat, F, (U64)-1, 0, 0), (U64)-9);
  CHECK(raw(Close, 1, 0, 0, 0), 0);
  CHECK(raw(OpenAt, 0, (U64)Path, 0, 0), 1);
  CHECK(raw(Fstat, 1, (U64)&Out.Status, 0, 0), 0);
  CHECK(verify_status(&Out.Status), 0);
  return 0;
}

U64 files_status_unsupported(U64 Mode, U64 Buffer) {
  if (Mode == 1)
    return raw(Fstat, 1, Buffer, 0, 0);
  U64 F = raw(OpenAt, 0, (U64)Path, 0, 0);
  if (Mode == 0)
    return raw(Fstat, F, Buffer, 0, 0);
  CHECK(raw(Protect, Buffer + (Mode == 2 ? 4096 : 0), 4096, 1, 0), 0);
  return raw(Fstat, F, Buffer + 4096 - 64, 0, 0);
}

U64 files_unsupported(U64 Mode) {
  if (Mode == 0)
    return raw(OpenAt, (U64)-100, (U64)Path, 0, 0);
  if (Mode == 1)
    return raw(OpenAt, (U64)-100, (U64)Path, 0100, 0);
  if (Mode == 2)
    return raw(OpenAt, (U64)-100, (U64) "fixture/data", 0, 0);
  if (Mode == 3)
    return raw(OpenAt, (U64)-100, (U64) "/fixture/../fixture/data", 0, 0);
  if (Mode == 4)
    return raw(Read, 0, (U64)Pages, 1, 0);
  if (Mode == 5)
    return raw(OpenAt, 0, (U64) "/fixture", 0, 0);
  U64 F = raw(OpenAt, 0, (U64)Path, 0, 0);
  return raw(Seek, F, 0, 3, 0);
}

#if defined(__ANDROID__)
extern int open(const char *, int, ...);
extern int open64(const char *, int, ...);
extern int openat(int, const char *, int, ...);
extern int openat64(int, const char *, int, ...);
extern I64 read(int, void *, U64);
extern int close(int);
extern I64 lseek(int, I64, int);
extern I64 lseek64(int, I64, int);
extern int fstat(int, struct FileStatus *);
extern int fstat64(int, struct FileStatus *);
extern I64 syscall(I64, ...);
extern int *__errno(void);
extern int pthread_create(U64 *, const void *, void *(*)(void *), void *);
extern int pthread_join(U64, void **);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);

static void *reader(void *Arg) {
  U64 FD = (U64)Arg;
  *__errno() = 82;
  U64 R = read(FD, Pages + 8, 2);
  return (void *)(R == 2 && *__errno() == 82 ? 0UL : 1UL);
}

U64 files_bionic(void) {
  *__errno() = 73;
  int (*volatile Open)(const char *, int, ...) = open;
  int F = Open(Path, 0, 0777);
  CHECK(F, 3);
  CHECK(*__errno(), 73);
  CHECK(raw(Read, F, (U64)Pages, 1, 0), 1);
  CHECK(syscall(63L, (U64)F, Pages + 1, 1UL), 1);
  U64 Thread;
  void *Value = (void *)1;
  CHECK(pthread_create(&Thread, 0, reader, (void *)(U64)F), 0);
  CHECK(pthread_join(Thread, &Value), 0);
  CHECK((U64)Value, 0);
  CHECK(Pages[8], 0x41);
  CHECK(Pages[9], 0x0a);
  CHECK(*__errno(), 73);
  CHECK(lseek(F, 0, 1), 4);
  CHECK(lseek64(F, -1, 2), 5);
  CHECK(read(F, Pages + 2, 8), 1);
  CHECK(Pages[0], 0);
  CHECK(Pages[1], 0xff);
  CHECK(Pages[2], 0x5a);
  CHECK(lseek64(F, 0x7fffffffffffffffL, 0), 0x7fffffffffffffffL);
  CHECK(read(F, Pages, 1), -1);
  CHECK(*__errno(), 22);
  CHECK(read(F, (void *)UserLimit, 0), 0);
  CHECK(*__errno(), 22);
  CHECK(read(F, (void *)UserLimit, 1), -1);
  CHECK(*__errno(), 14);
  CHECK(lseek64(F, 0, 1), 0x7fffffffffffffffL);
  CHECK(close(F), 0);
  CHECK(read(F, Pages, 1), -1);
  CHECK(*__errno(), 9);
  CHECK(open64(Path, 0), 3);
  CHECK(openat(-1, Path, 0), 4);
  CHECK(openat64(-1, Path, 0), 5);
  CHECK(open("/absent", 0), -1);
  CHECK(*__errno(), 2);
  return 0;
}

static void *status_reader(void *Arg) {
  struct FileStatus Status;
  *__errno() = 82;
  if (fstat64((U64)Arg, &Status) || verify_status(&Status) ||
      *__errno() != 82 || fstat(-1, &Status) != -1 || *__errno() != 9)
    return (void *)1;
  return 0;
}

U64 files_status_bionic(void) {
  struct FileStatus Status;
  *__errno() = 73;
  int F = open(Path, 0);
  CHECK(F, 3);
  CHECK(read(F, Pages, 1), 1);
  CHECK(fstat(F, &Status), 0);
  CHECK(verify_status(&Status), 0);
  CHECK(*__errno(), 73);
  CHECK(syscall(80L, (U64)F | 0x1234567800000000UL, &Status), 0);
  CHECK(verify_status(&Status), 0);
  CHECK(*__errno(), 73);
  U64 Thread;
  void *Value = (void *)1;
  CHECK(pthread_create(&Thread, 0, status_reader, (void *)(U64)F), 0);
  CHECK(pthread_join(Thread, &Value), 0);
  CHECK((U64)Value, 0);
  CHECK(*__errno(), 73);
  CHECK(lseek(F, 0, 1), 1);
  CHECK(fstat64(F, (struct FileStatus *)1), -1);
  CHECK(*__errno(), 14);
  CHECK(fstat(F, &Status), 0);
  CHECK(*__errno(), 14);
  CHECK(verify_status(&Status), 0);
  CHECK(close(F), 0);
  CHECK(fstat(F, (struct FileStatus *)1), -1);
  CHECK(*__errno(), 9);
  CHECK(open(Path, 0), F);
  CHECK(fstat64(F, &Status), 0);
  CHECK(verify_status(&Status), 0);
  return 0;
}

U64 files_status_dynamic(U64 Closed) {
  struct FileStatus Status;
  int F = open(Path, 0);
  CHECK(F, 3);
  void *Library = dlopen("libfiles.so", 2);
  if (!Library)
    return 99;
  int (*StatusCall)(int, struct FileStatus *) = dlsym(Library, "fstat64");
  if (!StatusCall)
    return 98;
  if (Closed)
    CHECK(dlclose(Library), 0);
  CHECK(StatusCall(F, &Status), 0);
  return verify_status(&Status);
}

U64 files_dynamic(U64 Closed) {
  void *Library = dlopen("libfiles.so", 2);
  if (!Library)
    return 99;
  int (*Open)(const char *, int, ...) = dlsym(Library, "open");
  if (!Open)
    return 98;
  if (Closed)
    CHECK(dlclose(Library), 0);
  return Open(Path, 0);
}
#else
void process_main(U64 *Stack) {
  if (Stack[0] != 2)
    __builtin_trap();
  char Mode = ((const char **)(Stack + 1))[1][0];
  U64 Status = Mode == 's'   ? files_sequence()
               : Mode == 'f' ? files_faults()
               : Mode == 'c' ? files_capacity()
               : Mode == 't' ? files_status()
                             : files_unsupported(Mode - '0');
  raw(Exit, Status, 0, 0, 0);
  __builtin_trap();
}
#endif
