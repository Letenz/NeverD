//===- linux_residency.c - Independent mincore boundary calls ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned long U64;
#if defined(__ANDROID__)
extern int mincore(void *, U64, unsigned char *);
extern long syscall(long, ...);
extern int *__errno(void);
static U64 raw(U64 Address, U64 Length, U64 Vector) {
  register U64 X0 __asm__("x0") = Address;
  register U64 X1 __asm__("x1") = Length;
  register U64 X2 __asm__("x2") = Vector;
  register U64 X8 __asm__("x8") = 232;
  __asm__ volatile("svc #0" : "+r"(X0) : "r"(X1), "r"(X2), "r"(X8) : "memory");
  return X0;
}
U64 residency_call(U64 Op, U64 Address, U64 Length, unsigned char *Vector,
                   int *Error) {
  *__errno() = 73;
  U64 Result;
  switch (Op) {
  case 0:
    Result = (U64)(long)mincore((void *)Address, Length, Vector);
    break;
  case 1:
    Result = (U64)syscall(232, Address, Length, Vector);
    break;
  default:
    Result = raw(Address, Length, (U64)Vector);
    break;
  }
  *Error = *__errno();
  return Result;
}
#else
extern U64 linux_service(U64, U64, U64, U64);
#if defined(__aarch64__)
enum { Mincore = 232, Write = 64, Exit = 93 };
#else
enum { Mincore = 27, Write = 1, Exit = 60 };
#endif
static U64 anonymous_pages(void) {
#if defined(__aarch64__)
  register U64 A0 __asm__("x0") = 0x60000000;
  register U64 A1 __asm__("x1") = 12288;
  register U64 A2 __asm__("x2") = 3;
  register U64 A3 __asm__("x3") = 0x22;
  register U64 A4 __asm__("x4") = ~0UL;
  register U64 A5 __asm__("x5") = 0;
  register U64 NR __asm__("x8") = 222;
  __asm__ volatile("svc #0"
                   : "+r"(A0)
                   : "r"(A1), "r"(A2), "r"(A3), "r"(A4), "r"(A5), "r"(NR)
                   : "memory", "cc");
  return A0;
#else
  register U64 A3 __asm__("r10") = 0x22;
  register U64 A4 __asm__("r8") = ~0UL;
  register U64 A5 __asm__("r9") = 0;
  U64 Result;
  __asm__ volatile("syscall"
                   : "=a"(Result)
                   : "a"(9UL), "D"(0x60000000UL), "S"(12288UL), "d"(3UL),
                     "r"(A3), "r"(A4), "r"(A5)
                   : "rcx", "r11", "memory", "cc");
  return Result;
#endif
}
static void check(U64 Address, U64 Length, U64 Vector, U64 Expected) {
  if (linux_service(Mincore, Address, Length, Vector) != Expected) {
    linux_service(Exit, 91, 0, 0);
    __builtin_trap();
  }
}
void process_main(U64 *Stack) {
  unsigned char Vector[32];
  for (unsigned I = 0; I != sizeof(Vector); ++I)
    Vector[I] = 0xa5;
  if (((const char **)(Stack + 1))[1][0] == 'o') {
    U64 Address = anonymous_pages();
    if (Address != 0x60000000UL)
      linux_service(Exit, 92, 0, 0);
    check(Address + 1, 12288, (U64)Vector, (U64)-22);
    check(Address, 0, (U64)Vector, 0);
    check(Address, 8193, (U64)(Vector + 3), 0);
    check(Address, 12288, (U64)(Vector + 8), 0);
    linux_service(Write, 1, (U64)Vector, sizeof(Vector));
    linux_service(Exit, 0, 0, 0);
  } else if (((const char **)(Stack + 1))[1][0] == 'm') {
    // A mapped stack page has no explicit residency observation.
    check((U64)Stack & ~4095UL, 4096, (U64)Vector, 0);
  } else {
    check(1, 0, ~0UL, (U64)-22);
    check(~4095UL, 4096, ~0UL, (U64)-12);
    check(0, 4097, ~0UL, (U64)-14);
    check(0, 1, 0, (U64)-12);
    check(0, 4097, (U64)Vector, (U64)-12);
    check(0, 0, 0, 0);
    check(0, 0, (U64)Vector, 0);
    linux_service(Write, 1, (U64)Vector, sizeof(Vector));
    linux_service(Exit, 0, 0, 0);
  }
  __builtin_trap();
}
#endif
