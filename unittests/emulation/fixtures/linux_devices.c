//===- linux_devices.c - Original explicit device transaction callers -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent raw Linux and Bionic callers with byte-level expectations.
//===----------------------------------------------------------------------===//
typedef unsigned long U64;
typedef long I64;
typedef unsigned int U32;
#if defined(__aarch64__)
enum {
  OpenAt = 56,
  Close = 57,
  Ioctl = 29,
  Exit = 93,
  Protect = 226,
  Map = 222,
  Unmap = 215,
  Dup = 23,
  Dup3 = 24,
  Read = 63,
  Write = 64,
  WriteV = 66,
  Sleep = 101
};
#else
enum {
  OpenAt = 257,
  Close = 3,
  Ioctl = 16,
  Exit = 60,
  Protect = 10,
  Map = 9,
  Unmap = 11,
  Dup = 32,
  Dup2 = 33,
  Dup3 = 292,
  Read = 0,
  Write = 1,
  WriteV = 20,
  Sleep = 35
};
#endif

static U64 raw6(U64 Number, U64 A, U64 B, U64 C, U64 D, U64 E, U64 F) {
#if defined(__aarch64__)
  register U64 N __asm__("x8") = Number;
  register U64 X0 __asm__("x0") = A;
  register U64 X1 __asm__("x1") = B;
  register U64 X2 __asm__("x2") = C;
  register U64 X3 __asm__("x3") = D;
  register U64 X4 __asm__("x4") = E;
  register U64 X5 __asm__("x5") = F;
  __asm__ volatile("svc #0"
                   : "+r"(X0)
                   : "r"(N), "r"(X1), "r"(X2), "r"(X3), "r"(X4), "r"(X5)
                   : "memory", "cc");
  return X0;
#else
  U64 Result;
  register U64 R10 __asm__("r10") = D;
  register U64 R8 __asm__("r8") = E;
  register U64 R9 __asm__("r9") = F;
  __asm__ volatile("syscall"
                   : "=a"(Result)
                   : "a"(Number), "D"(A), "S"(B), "d"(C), "r"(R10), "r"(R8),
                     "r"(R9)
                   : "memory", "cc", "rcx", "r11");
  return Result;
#endif
}

static U64 raw(U64 Number, U64 A, U64 B, U64 C) {
  return raw6(Number, A, B, C, 0, 0, 0);
}

#ifdef __ANDROID__
extern int open(const char *, int, ...);
extern int ioctl(int, int, ...);
extern I64 syscall(I64, ...);
extern int *__errno(void);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);
extern void *mmap(void *, U64, int, int, int, I64);
extern int dup(int);
extern int dup2(int, int);
extern int dup3(int, int, int);
#endif

static const char Path[] = "/fixture/control";
static I64 invoke(U64 Route, U64 FD, U64 Request, U64 Argument) {
#ifdef __ANDROID__
  if (Route == 1)
    return ioctl((int)FD, (int)Request, Argument);
  if (Route == 2)
    return syscall(Ioctl, FD, Request, Argument);
  if (Route >= 3) {
    void *Provider = dlopen("libdevice.so", 2);
    int (*Call)(int, int, ...) = dlsym(Provider, "ioctl");
    if (!Provider || !Call)
      return -99;
    if (Route == 4)
      dlclose(Provider);
    I64 Result = Call((int)FD, (int)Request, Argument);
    if (Route == 3)
      dlclose(Provider);
    return Result;
  }
#endif
  return (I64)raw(Ioctl, FD | 0x1122334400000000UL,
                  Request | 0x5566778800000000UL, Argument);
}
#define CHECK(A, B)                                                            \
  do {                                                                         \
    if ((U64)(A) != (U64)(B))                                                  \
      return __LINE__;                                                         \
  } while (0)

static U64 exchange(U64 Route, U64 FD) {
  unsigned char Packet[24], Payload[8];
  for (unsigned I = 0; I != sizeof(Packet); ++I)
    Packet[I] = 0xa5;
  for (unsigned I = 0; I != sizeof(Payload); ++I)
    Payload[I] = 0xc7;
  Packet[0] = 0x31;
  Packet[1] = 0x32;
  Payload[1] = 0x55;
  Payload[2] = 0x56;
  U64 Pointer = (U64)Payload;
  for (unsigned I = 0; I != 8; ++I)
    Packet[8 + I] = Pointer >> (8 * I);
  CHECK(invoke(Route, FD, 0xa1000001U, (U64)Packet), 37);
  for (unsigned I = 0; I != sizeof(Packet); ++I) {
    unsigned char Expected = I == 0   ? 0x52
                             : I == 1 ? 0x53
                             : I >= 8 && I < 16
                                 ? (unsigned char)(Pointer >> (8 * (I - 8)))
                                 : 0xa5;
    CHECK(Packet[I], Expected);
  }
  for (unsigned I = 0; I != sizeof(Payload); ++I)
    CHECK(Payload[I], I == 1 ? 0x90 : I == 2 ? 0x91 : 0xc7);
  return 0;
}

U64 devices_sequence(U64 Route) {
  U64 FD = raw(OpenAt, (U64)-100, (U64)Path, 2 | 02000000);
  CHECK(FD, 3);
  U64 Other = raw(OpenAt, 0xdeadbeef, (U64)Path, 2);
  CHECK(Other, 4);
#ifdef __ANDROID__
  *__errno() = 22;
#endif
  U64 Error = exchange(Route, FD);
  if (Error)
    return Error;
#ifdef __ANDROID__
  CHECK(*__errno(), 22);
#endif
  CHECK(invoke(Route, FD, 0xa1000002U, 0x8877665544332211UL), Route ? -1 : -13);
#ifdef __ANDROID__
  CHECK(*__errno(), Route ? 13 : 22);
#endif
  Error = exchange(Route, Other);
  if (Error)
    return Error;
  CHECK(raw(Close, FD, 0, 0), 0);
  CHECK(invoke(Route, FD, 0, 1), Route ? -1 : -9);
#ifdef __ANDROID__
  CHECK(*__errno(), Route ? 9 : 22);
#endif
  CHECK(raw(OpenAt, (U64)-100, (U64)Path, 2), 3);
  Error = exchange(Route, 3);
  if (Error)
    return Error;
  CHECK(raw(Close, 3, 0, 0), 0);
  CHECK(raw(Close, Other, 0, 0), 0);
  return 0;
}

U64 devices_pending(U64 Route, U64 Buffer) {
  U64 FD = raw(OpenAt, (U64)-100, (U64)Path, 2);
  return invoke(Route, FD, 0xa1000001U, Buffer);
}
U64 devices_pending_readonly(U64 Route, U64 Buffer) {
  CHECK(raw(Protect, Buffer, 4096, 1), 0);
  return devices_pending(Route, Buffer);
}

static U64 mapping(U64 Route, U64 FD, U64 Length, U64 Offset, int Prot) {
#ifdef __ANDROID__
  if (Route == 1)
    return (U64)mmap(0, Length, Prot, 1, (int)FD, (I64)Offset);
  if (Route == 2)
    return (U64)syscall(Map, 0UL, Length, (U64)Prot, 1UL, FD, Offset);
  if (Route == 3) {
    void *Provider = dlopen("libdevice.so", 2);
    void *(*Call)(void *, U64, int, int, int, I64) = dlsym(Provider, "mmap");
    if (!Provider || !Call)
      return (U64)-99;
    U64 Result = (U64)Call(0, Length, Prot, 1, (int)FD, (I64)Offset);
    dlclose(Provider);
    return Result;
  }
#endif
  return raw6(Map, 0, Length, Prot, 1, FD | 0x1122334400000000UL, Offset);
}

U64 devices_mapping(U64 Route) {
  U64 FD = raw(OpenAt, (U64)-100, (U64)Path, 2);
  CHECK(FD, 3);
  CHECK(mapping(Route, FD, 0, 4096, 1), Route ? -1 : -22);
  CHECK(mapping(Route, FD, 4096, 4097, 1), Route ? -1 : -22);
  CHECK(mapping(Route, FD, 4096, 4096, 3), Route ? -1 : -13);
#ifdef __ANDROID__
  CHECK(*__errno(), Route ? 13 : 0);
  *__errno() = 22;
#endif
  U64 A = mapping(Route, FD, 3 * 4096 - 1, 4096, 1);
  if (A >= (U64)-4095)
    return __LINE__;
  CHECK(raw(Close, FD, 0, 0), 0);
  CHECK(*(volatile unsigned char *)A, 0x31);
  CHECK(*(volatile unsigned char *)(A + 3 * 4096 - 1), 0x33);
  CHECK(mapping(Route, FD, 4096, 4096, 1), Route ? -1 : -9);
  CHECK(raw(OpenAt, (U64)-100, (U64)Path, 2), 3);
  U64 B = mapping(Route, 3, 2 * 4096, 2 * 4096, 0);
  if (B >= (U64)-4095 || A == B)
    return __LINE__;
  CHECK(raw(Close, 3, 0, 0), 0);
  CHECK(raw(Protect, B, 2 * 4096, 3), -13);
  CHECK(raw(Protect, B, 2 * 4096, 1), 0);
  CHECK(raw(Unmap, A + 4096, 4096, 0), 0);
  CHECK(*(volatile unsigned char *)B, 0x32);
  CHECK(*(volatile unsigned char *)(B + 4096), 0x33);
  CHECK(raw(Unmap, B, 2 * 4096, 0), 0);
  CHECK(*(volatile unsigned char *)(A + 2 * 4096), 0x33);
  CHECK(raw(Unmap, A, 3 * 4096, 0), 0);
  return 0;
}

U64 devices_mapping_fault(U64 Mode) {
  U64 FD = raw(OpenAt, (U64)-100, (U64)Path, 2);
  U64 A = mapping(0, FD, 4096, 4096, Mode == 0 ? 0 : 1);
  CHECK(raw(Close, FD, 0, 0), 0);
  if (Mode == 1)
    *(volatile unsigned char *)A = 0xff;
  if (Mode == 2)
    CHECK(raw(Unmap, A, 4096, 0), 0);
  return *(volatile unsigned char *)A;
}

static U64 duplicate(U64 Route, U64 FD, U64 Target, U64 Flags) {
#ifdef __ANDROID__
  if (Route == 1)
    return Flags == (U64)-1 ? dup((int)FD) : dup3((int)FD, (int)Target, Flags);
  if (Route == 2)
    return Flags == (U64)-1 ? syscall(Dup, FD)
                            : syscall(Dup3, FD, Target, Flags);
  if (Route == 3) {
    void *P = dlopen("libdevice.so", 2);
    int (*One)(int) = dlsym(P, "dup");
    int (*Three)(int, int, int) = dlsym(P, "dup3");
    if (!P || !One || !Three)
      return (U64)-99;
    I64 R = Flags == (U64)-1 ? One(FD) : Three(FD, Target, Flags);
    dlclose(P);
    return R;
  }
#endif
  return Flags == (U64)-1 ? raw(Dup, FD | 0x123400000000UL, 0, 0)
                          : raw(Dup3, FD, Target | 0x123400000000UL, Flags);
}

U64 devices_duplicates(U64 Route) {
  CHECK(raw(OpenAt, (U64)-100, (U64)Path, 2), 3);
  CHECK(duplicate(Route, 3, 0, (U64)-1), 4);
  CHECK(duplicate(Route, 3, 6, 02000000), 6);
  CHECK(duplicate(Route, 3, 3, 0), Route ? -1 : -22);
  CHECK(duplicate(Route, 3, 5, 1), Route ? -1 : -22);
  CHECK(duplicate(Route, 99, 5, 0), Route ? -1 : -9);
  CHECK(duplicate(Route, 3, 256, 0), Route ? -1 : -9);
  CHECK(exchange(Route, 4), 0);
  CHECK(raw(Close, 3, 0, 0), 0);
  CHECK(raw(Close, 4, 0, 0), 0);
  CHECK(invoke(Route, 6, 0xa1000002, 0x8877665544332211UL), Route ? -1 : -13);
  CHECK(raw(OpenAt, (U64)-100, (U64)Path, 2), 3);
  CHECK(exchange(Route, 3), 0);
  CHECK(raw(Close, 6, 0, 0), 0);
  CHECK(raw(Close, 3, 0, 0), 0);

  unsigned char Byte = 0;
  CHECK(raw(OpenAt, (U64)-100, (U64) "/fixture/bytes", 0), 3);
  CHECK(raw(Read, 3, (U64)&Byte, 1), 1);
  CHECK(Byte, 'a');
  CHECK(duplicate(Route, 3, 0, (U64)-1), 4);
  CHECK(raw(Read, 4, (U64)&Byte, 1), 1);
  CHECK(Byte, 'b');
  CHECK(raw(OpenAt, (U64)-100, (U64) "/fixture/bytes", 0), 5);
  CHECK(raw(Read, 5, (U64)&Byte, 1), 1);
  CHECK(Byte, 'a');
#ifdef __ANDROID__
  CHECK(dup2(4, 5), 5);
  CHECK(dup2(5, 5), 5);
  CHECK(dup2(99, 99), -1);
#elif defined(__x86_64__)
  CHECK(raw(Dup2, 4, 5, 0), 5);
  CHECK(raw(Dup2, 5, 5, 0), 5);
  CHECK(raw(Dup2, 99, 99, 0), -9);
#else
  CHECK(duplicate(Route, 4, 5, 0), 5);
#endif
  CHECK(raw(Close, 3, 0, 0), 0);
  CHECK(raw(Close, 4, 0, 0), 0);
  CHECK(raw(Read, 5, (U64)&Byte, 1), 1);
  CHECK(Byte, 'c');
  CHECK(raw(Close, 5, 0, 0), 0);
  CHECK(duplicate(Route, 1, 0, (U64)-1), 3);
  CHECK(duplicate(Route, 2, 1, 0), 1);
  CHECK(raw(Write, 3, (U64) "out", 3), 3);
  U64 Vector[2] = {(U64) "err", 3};
  CHECK(raw(WriteV, 1, (U64)Vector, 1), 3);
  return 0;
}

static U64 sleep_ms(U64 Count) {
  U64 Request[2] = {0, Count * 1000000};
  return raw(Sleep, (U64)Request, 0, 0);
}

U64 devices_busy_poll(void) {
  CHECK(raw(OpenAt, (U64)-100, (U64)Path, 2), 3);
  U64 A = mapping(0, 3, 4096, 4096, 1);
  while (*(volatile unsigned char *)A != 0x9a) {
  }
  return 0;
}

U64 devices_updates(void) {
  CHECK(raw(OpenAt, (U64)-100, (U64)Path, 2), 3);
  U64 A = mapping(0, 3, 3 * 4096, 4096, 1);
  U64 B = mapping(0, 3, 2 * 4096, 2 * 4096, 0);
  CHECK(*(volatile unsigned char *)A, 0x61); // due at time zero
  CHECK(sleep_ms(1), 0);
  CHECK(*(volatile unsigned char *)(A + 4095), 0xaa);
  CHECK(raw(Protect, B, 2 * 4096, 1), 0);
  CHECK(*(volatile unsigned char *)B, 0xee); // same-time last writer
  CHECK(*(volatile unsigned char *)(B + 1), 0xcc);
  CHECK(raw(Unmap, A + 4096, 4096, 0), 0);
  CHECK(raw6(Map, A + 4096, 4096, 3, 0x22, (U64)-1, 0), A + 4096);
  *(volatile unsigned char *)(A + 4096) = 0x77;
  CHECK(sleep_ms(1), 0);
  CHECK(*(volatile unsigned char *)(A + 4096), 0x77);
  CHECK(*(volatile unsigned char *)B, 0xdd);
  CHECK(*(volatile unsigned char *)(B + 1), 0xcc);
  CHECK(raw(Close, 3, 0, 0), 0);
  CHECK(raw(Unmap, A, 3 * 4096, 0), 0);
  CHECK(raw(Unmap, B, 2 * 4096, 0), 0);
  CHECK(sleep_ms(1), 0); // producer advances with no live mapping or FD
  CHECK(raw(OpenAt, (U64)-100, (U64)Path, 2), 3);
  U64 C = mapping(0, 3, 3 * 4096, 4096, 1);
  CHECK(*(volatile unsigned char *)C, 0x61);
  CHECK(*(volatile unsigned char *)(C + 4095), 0xaa);
  CHECK(*(volatile unsigned char *)(C + 4096), 0xdd);
  CHECK(*(volatile unsigned char *)(C + 8192), 0x88);
  CHECK(*(volatile unsigned char *)(C + 8193), 0x33);
  return 0;
}

enum {
  CreateContext = 0xc0080913U,
  DestroyContext = 0x40040914U,
  ReadTimestamp = 0xc00c0916U
};
U64 devices_contexts(U64 Route) {
  CHECK(raw(OpenAt, (U64)-100, (U64)Path, 2), 3);
  CHECK(raw(OpenAt, (U64)-100, (U64)Path, 2), 4);
  U32 Create[2] = {0, 0xffffffffU};
  CHECK(invoke(Route, 3, CreateContext, (U64)Create), Route ? -1 : -22);
  Create[0] = 0x12;
  CHECK(invoke(Route, 3, CreateContext, (U64)Create), 0);
  CHECK(Create[1], 1);
  CHECK(Create[0], 0x8052);
  CHECK(invoke(Route, 3, CreateContext, (U64)Create), 0);
  CHECK(Create[1], 2);
  CHECK(invoke(Route, 4, CreateContext, (U64)Create), 0);
  CHECK(Create[1], 3);
  CHECK(invoke(Route, 4, CreateContext, (U64)Create), Route ? -1 : -28);
  CHECK(Create[1], 3);
  U32 Query[3] = {3, 2, 0xffffffffU};
  CHECK(invoke(Route, 3, ReadTimestamp, (U64)Query), Route ? -1 : -22);
  CHECK(Query[2], 0xffffffffU);
  Query[0] = 1;
  CHECK(invoke(Route, 3, ReadTimestamp, (U64)Query), 0);
  CHECK(Query[2], 0);
  Query[0] = 2;
  Query[1] = 3; // no producer updates context 2; its queue remains empty
  CHECK(invoke(Route, 3, ReadTimestamp, (U64)Query), 0);
  CHECK(Query[2], 0);
  Query[1] = 99;
  Query[2] = 0xdeadbeef;
  CHECK(invoke(Route, 3, ReadTimestamp, (U64)Query), Route ? -1 : -22);
  CHECK(Query[2], 0); // invalid type still copies a sanitized timestamp
  Query[0] = 1;
  Query[1] = 2;
  U64 A = mapping(Route, 3, 4096, 4096, 1);
  U64 B = mapping(Route, 4, 4096, 4096, 0);
  CHECK(*(volatile U32 *)(A + 40), 0);
  CHECK(*(volatile U32 *)(A + 44), 0xa5a5a5a5U);
  CHECK(*(volatile U32 *)(A + 48), 0);
  CHECK(duplicate(Route, 3, 0, (U64)-1), 5);
  CHECK(raw(Close, 3, 0, 0), 0);
  CHECK(invoke(Route, 3, ReadTimestamp, (U64)Query), Route ? -1 : -9);
  CHECK(sleep_ms(1), 0);
  CHECK(invoke(Route, 5, ReadTimestamp, (U64)Query), 0);
  CHECK(Query[2], 5);
  Query[1] = 1;
  CHECK(invoke(Route, 5, ReadTimestamp, (U64)Query), 0);
  CHECK(Query[2], 7);
  CHECK(*(volatile U32 *)(A + 40), 7);
  CHECK(raw(Protect, B, 4096, 1), 0);
  CHECK(*(volatile U32 *)(B + 48), 5);
  U32 ID = 2;
  CHECK(invoke(Route, 5, DestroyContext, (U64)&ID), 0);
  CHECK(invoke(Route, 5, DestroyContext, (U64)&ID), Route ? -1 : -22);
  CHECK(invoke(Route, 4, CreateContext, (U64)Create), 0);
  CHECK(Create[1], 2);
  CHECK(sleep_ms(1), 0); // explicit producer retires the outstanding timestamp
  CHECK(raw(Close, 5, 0, 0), 0); // VMA still retains context 1
  CHECK(invoke(Route, 4, CreateContext, (U64)Create), Route ? -1 : -28);
  CHECK(raw(Unmap, A, 4096, 0), 0); // last reference releases context 1
  CHECK(invoke(Route, 4, CreateContext, (U64)Create), 0);
  CHECK(Create[1], 1);
  CHECK(*(volatile U32 *)(B + 40), 0); // recycled idle context
  CHECK(*(volatile U32 *)(B + 48), 0);
  Query[1] = 2;
  CHECK(invoke(Route, 4, ReadTimestamp, (U64)Query), 0);
  CHECK(Query[2], 0);
  return 0;
}

U64 devices_context_unsupported(U64 Mode, U64 Buffer) {
  CHECK(raw(OpenAt, (U64)-100, (U64)Path, 2), 3);
  U32 *Packet = (U32 *)Buffer;
  Packet[0] = Mode == 0 ? 0x20012 : 0x12;
  Packet[1] = 0xffffffffU;
  if (Mode == 1)
    CHECK(raw(Protect, Buffer, 4096, 1), 0);
  U64 R = invoke(0, 3, CreateContext, Buffer);
  if (R)
    return R;
  if (Mode >= 3) {
    U64 A = Mode == 6 ? mapping(0, 3, 4096, 4096, 1) : 0;
    if (Mode == 6)
      CHECK(raw(Close, 3, 0, 0), 0);
    CHECK(sleep_ms(1), 0);
    if (Mode == 3)
      return invoke(0, 3, DestroyContext, Buffer + 4);
    if (Mode == 4)
      return raw(Close, 3, 0, 0);
    if (Mode == 5)
      return raw(Dup3, 1, 3, 0);
    return raw(Unmap, A, 4096, 0);
  }
  Packet[0] = Packet[1];
  Packet[1] = 3;
  Packet[2] = 0xdeadbeefU;
  return invoke(0, 3, ReadTimestamp, Buffer);
}

struct GPUMap {
  int fd;
  U64 gpu, length, offset, host;
  U32 type, flags;
};
struct GPUSubmit {
  U32 context;
  U64 commands;
  U32 words, timestamp, flags;
};
_Static_assert(sizeof(struct GPUMap) == 48, "LP64 GPU mapping");
_Static_assert(sizeof(struct GPUSubmit) == 32, "LP64 GPU submission");
U64 devices_gpu_commands(U64 Route) {
  CHECK(raw(OpenAt, (U64)-100, (U64)Path, 2), 3);
  U32 context[2] = {0x12, 0};
  CHECK(invoke(Route, 3, CreateContext, (U64)context), 0);
  U64 store = mapping(0, 3, 4096, 4096, 1);
  U64 code = raw6(Map, 0, 4096, 3, 0x22, (U64)-1, 0);
  U64 data = raw6(Map, 0, 4096, 3, 0x22, (U64)-1, 0);
  if (code > (U64)-4096 || data > (U64)-4096)
    return __LINE__;
  struct GPUMap cmd = {-1, 0, 4096, 0, code, 2, 0x01000000};
  struct GPUMap dst = {-1, 0, 4096, 0, data, 2, 0};
  CHECK(invoke(Route, 3, 0xc0300915, (U64)&cmd), 0);
  CHECK(invoke(Route, 3, 0xc0300915, (U64)&dst), 0);
  CHECK(cmd.flags, 0x01000060);
  CHECK(dst.flags, 0x60);
  U32 *p = (U32 *)code, *d = (U32 *)data;
  for (U32 i = 0; i != 1024; ++i)
    d[i] = 0x8899aabb;
  p[0] = 0xc0021000;
  p[1] = 0xffffffff;
  p[2] = 0xc0013d00;
  p[3] = 1; /* NOP payload */
  p[4] = 0xc0013d00;
  p[5] = (U32)dst.gpu + 4;
  p[6] = 0x11223344;
  p[7] = 0xc0013d00;
  p[8] = (U32)dst.gpu + 4092;
  p[9] = 0xff00ff00;
  struct GPUSubmit submit = {context[1], cmd.gpu, 10, 0xdeadbeef, 0};
  CHECK(invoke(Route, 3, 0xc0200910, (U64)&submit), 0);
  CHECK(submit.timestamp, 1);
  CHECK(d[0], 0x8899aabb);
  CHECK(d[1], 0x11223344);
  CHECK(d[2], 0x8899aabb);
  CHECK(d[1022], 0x8899aabb);
  CHECK(d[1023], 0xff00ff00);
  CHECK(*(volatile U32 *)(store + context[1] * 40), 1);
  CHECK(*(volatile U32 *)(store + context[1] * 40 + 8), 1);
  /* GPU pins cannot follow a recycled CPU virtual address. */
  CHECK(raw(Unmap, code, 4096, 0), 0);
  CHECK(raw6(Map, code, 4096, 3, 0x32, (U64)-1, 0), code);
  p[0] = 0xffffffff;
  d[1] = 0;
  CHECK(invoke(Route, 3, 0xc0200910, (U64)&submit), 0);
  CHECK(submit.timestamp, 2);
  CHECK(d[1], 0x11223344);
  CHECK(p[0], 0xffffffff);
  U32 query[3] = {context[1], 3, 0xdeadbeef};
  CHECK(invoke(Route, 3, ReadTimestamp, (U64)query), 0);
  CHECK(query[2], 2);
  U32 wait[3] = {context[1], 2, 0};
  CHECK(invoke(Route, 3, 0x400c0907, (U64)wait), 0);
  /* Independently opened descriptions share the process GPU address space. */
  CHECK(raw(OpenAt, (U64)-100, (U64)Path, 2), 4);
  CHECK(raw(Close, 3, 0, 0), 0);
  CHECK(invoke(Route, 4, 0x40080921, (U64)&cmd.gpu), 0);
  CHECK(invoke(Route, 4, 0x40080921, (U64)&dst.gpu), 0);
  CHECK(raw(Unmap, store, 4096, 0), 0);
  CHECK(raw(Unmap, code, 4096, 0), 0);
  CHECK(raw(Unmap, data, 4096, 0), 0);
  CHECK(raw(Close, 4, 0, 0), 0);
  return 0;
}
U64 devices_gpu_rejected(U64 Mode, U64 Buffer) {
  CHECK(raw(OpenAt, (U64)-100, (U64)Path, 2), 3);
  U32 context[2] = {0x12, 0};
  CHECK(invoke(0, 3, CreateContext, (U64)context), 0);
  CHECK(raw6(Map, 0x22000000, 4096, 1, 1, 3, 4096), 0x22000000);
  U64 code = raw6(Map, 0, 4096, 3, 0x22, (U64)-1, 0);
  struct GPUMap cmd = {-1, 0, 4096, 0, code, 2, 0};
  struct GPUMap dst = {-1, 0, 4096, 0, Buffer, 2, Mode == 1 ? 0x01000000 : 0};
  CHECK(invoke(0, 3, 0xc0300915, (U64)&cmd), 0);
  CHECK(invoke(0, 3, 0xc0300915, (U64)&dst), 0);
  U32 *p = (U32 *)code;
  p[0] = 0xc0013d00;
  p[1] = (U32)(Mode == 2 ? cmd.gpu : dst.gpu);
  p[2] = 0x12345678;
  p[3] = Mode == 0 ? 0xc0009900 : 0xc0001000;
  p[4] = 0;
  struct GPUSubmit *submit = (struct GPUSubmit *)(Buffer + 4096);
  *submit =
      (struct GPUSubmit){context[1], cmd.gpu, Mode == 3 ? 2 : 5, 0xdeadbeef, 0};
  return invoke(0, 3, 0xc0200910, (U64)submit);
}
#ifdef __ANDROID__
extern int pthread_create(U64 *, const void *, void *(*)(void *), void *);
extern int pthread_join(U64, void **);
static void *poll_page(void *Page) {
  while (*(volatile unsigned char *)Page != 0x9a) {
  }
  return Page;
}
U64 devices_thread_updates(void) {
  CHECK(raw(OpenAt, (U64)-100, (U64)Path, 2), 3);
  U64 A = mapping(0, 3, 4096, 4096, 1), Thread = 0;
  CHECK(pthread_create(&Thread, 0, poll_page, (void *)A), 0);
  CHECK(raw(Close, 3, 0, 0), 0);
  CHECK(sleep_ms(1), 0); // worker remains runnable and advances the same clock
  void *Returned = 0;
  CHECK(pthread_join(Thread, &Returned), 0);
  CHECK(Returned, A);
  CHECK(*(volatile unsigned char *)A, 0x9a);
  return 0;
}
U64 devices_open_error(void) {
  *__errno() = 22;
  CHECK(open(Path, 2), -1);
  CHECK(*__errno(), 13);
  CHECK(raw(OpenAt, (U64)-100, (U64)Path, 2), -13);
  return 0;
}
#else
void process_main(U64 *Stack) {
  if (Stack[0] > 1) {
    char Mode = ((const char *)Stack[2])[0];
    if (Mode == 'd' || Mode == 'u' || Mode == 'k' || Mode == 'b' ||
        Mode == 'g') {
      U64 Status = Mode == 'd'   ? devices_duplicates(0)
                   : Mode == 'u' ? devices_updates()
                   : Mode == 'k' ? devices_contexts(0)
                   : Mode == 'g' ? devices_gpu_commands(0)
                                 : devices_busy_poll();
      raw(Exit, Status, 0, 0);
      __builtin_trap();
    }
  }
  U64 Status = devices_sequence(0);
  if (!Status)
    Status = devices_mapping(0);
  raw(Exit, Status, 0, 0);
  __builtin_trap();
}
#endif
