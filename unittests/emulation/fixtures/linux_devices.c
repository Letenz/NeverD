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
enum { OpenAt = 56, Close = 57, Ioctl = 29, Exit = 93, Protect = 226 };
#else
enum { OpenAt = 257, Close = 3, Ioctl = 16, Exit = 60, Protect = 10 };
#endif

static U64 raw(U64 Number, U64 A, U64 B, U64 C) {
#if defined(__aarch64__)
  register U64 N __asm__("x8") = Number;
  register U64 X0 __asm__("x0") = A;
  register U64 X1 __asm__("x1") = B;
  register U64 X2 __asm__("x2") = C;
  __asm__ volatile("svc #0"
                   : "+r"(X0)
                   : "r"(N), "r"(X1), "r"(X2)
                   : "memory", "cc");
  return X0;
#else
  U64 Result;
  __asm__ volatile("syscall"
                   : "=a"(Result)
                   : "a"(Number), "D"(A), "S"(B), "d"(C)
                   : "memory", "cc", "rcx", "r11");
  return Result;
#endif
}

#ifdef __ANDROID__
extern int open(const char *, int, ...);
extern int ioctl(int, int, ...);
extern I64 syscall(I64, ...);
extern int *__errno(void);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);
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
  raw(Exit, Status, 0, 0);
  __builtin_trap();
}
#endif
