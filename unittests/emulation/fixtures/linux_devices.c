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
  Unmap = 215
};
#else
enum {
  OpenAt = 257,
  Close = 3,
  Ioctl = 16,
  Exit = 60,
  Protect = 10,
  Map = 9,
  Unmap = 11
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
#ifdef __ANDROID__
U64 devices_open_error(void) {
  *__errno() = 22;
  CHECK(open(Path, 2), -1);
  CHECK(*__errno(), 13);
  CHECK(raw(OpenAt, (U64)-100, (U64)Path, 2), -13);
  return 0;
}
#else
void process_main(U64 *Stack) {
  (void)Stack;
  U64 Status = devices_sequence(0);
  if (!Status)
    Status = devices_mapping(0);
  raw(Exit, Status, 0, 0);
  __builtin_trap();
}
#endif
