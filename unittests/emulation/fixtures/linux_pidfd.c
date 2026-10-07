//===- linux_pidfd.c - Independent process-descriptor workloads ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned long U64;
extern U64 linux_service(U64, U64, U64, U64);
#if defined(__aarch64__)
enum {
  Mmap = 222,
  Mprotect = 226,
  Munmap = 215,
  GetPID = 172,
  OpenAt = 56,
  Close = 57,
  Read = 63,
  Write = 64,
  WriteV = 66,
  Seek = 62,
  Exit = 93
};
#else
enum {
  Mmap = 9,
  Mprotect = 10,
  Munmap = 11,
  GetPID = 39,
  OpenAt = 257,
  Close = 3,
  Read = 0,
  Write = 1,
  WriteV = 20,
  Seek = 8,
  Exit = 60
};
#endif
// The metadata ordering case needs all six anonymous-mmap arguments.
static U64 call6(U64 number, U64 a0, U64 a1, U64 a2, U64 a3, U64 a4, U64 a5) {
#if defined(__aarch64__)
  register U64 x0 __asm__("x0") = a0;
  register U64 x1 __asm__("x1") = a1;
  register U64 x2 __asm__("x2") = a2;
  register U64 x3 __asm__("x3") = a3;
  register U64 x4 __asm__("x4") = a4;
  register U64 x5 __asm__("x5") = a5;
  register U64 x8 __asm__("x8") = number;
  __asm__ volatile("svc #0"
                   : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                   : "memory", "cc");
  return x0;
#else
  register U64 r10 __asm__("r10") = a3;
  register U64 r8 __asm__("r8") = a4;
  register U64 r9 __asm__("r9") = a5;
  U64 result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(number), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8),
                     "r"(r9)
                   : "rcx", "r11", "memory", "cc");
  return result;
#endif
}
typedef struct {
  const void *base;
  U64 length;
} Vector;
#if defined(__aarch64__)
static const U64 UserLimit = 0x0001000000000000UL;
#else
static const U64 UserLimit = 0x0000800000000000UL;
#endif
static void finish(U64 status) {
  linux_service(Exit, status, 0, 0);
  __builtin_trap();
}
static void require(int condition, unsigned code) {
  if (!condition)
    finish(code);
}
static U64 pidfd(U64 pid, U64 flags) {
  return linux_service(434, pid, flags, 0);
}
void process_main(U64 *stack) {
  require(stack[0] == 3, 90);
  const char **arguments = (const char **)(stack + 1);
  char mode = arguments[1][0];
  // Old imports validate the original extent; the newer one-buffer path
  // caps it first. No payload exists here, and the output budget is one byte.
  const Vector capped = {(void *)(UserLimit - 0x7ffff000UL), 0x80000000UL};
  if (mode == 'b') {
    require(linux_service(WriteV, 1, (U64)&capped, 1) == (U64)-14, 50);
    finish(0);
  }
  U64 self = linux_service(GetPID, 0, 0, 0);
  if (mode == 'v') {
    int single_buffer = arguments[2][0] == '1';
    U64 fd = pidfd(self, 0);
    require(fd == 3, 51);
    U64 region = call6(Mmap, 0, 8192, 3, 0x22, ~0UL, 0);
    require(region && region < UserLimit, 52);
    Vector *tail = (Vector *)(region + 4096 - sizeof(Vector));
    tail->base = (void *)1;
    tail->length = ~0UL;
    require(linux_service(Mprotect, region + 4096, 4096, 0) == 0, 53);
    U64 expected = single_buffer ? (U64)-22 : (U64)-14;
    require(linux_service(WriteV, fd, (U64)tail, 2) == expected, 54);
    require(linux_service(WriteV, 1, (U64)tail, 2) == expected, 55);
    require(linux_service(WriteV, 2, (U64)tail, 2) == expected, 56);
    require(linux_service(WriteV, fd, (U64)&capped, 1) == expected, 57);
    const Vector multiple[] = {capped, {(void *)1, 0}};
    require(linux_service(WriteV, fd, (U64)multiple, 2) == (U64)-14, 58);
    require(linux_service(Close, fd, 0, 0) == 0, 59);
    require(linux_service(Munmap, region, 8192, 0) == 0, 60);
    finish(0);
  }
  if (mode == 'u') {
    pidfd(2000, 0);
    finish(91);
  }
  if (mode == 'm') {
    pidfd(self, 0);
    finish(91);
  }
  if (mode == 't') {
    U64 r = pidfd(self, 0x880);
    if (arguments[2][0] == '1') {
      require(r == 3, 1);
      require(linux_service(Close, r, 0, 0) == 0, 2);
    } else
      require(r == (U64)-22, 3);
    finish(0);
  }
  require(pidfd(0, 0) == (U64)-22, 4);
  require(pidfd(~0UL, 0) == (U64)-22, 5);
  require(pidfd(0x80000000UL, 0) == (U64)-22, 6);
  require(pidfd(2000, 1) == (U64)-22, 7);
  if (mode == 'd') {
    require(pidfd(self, 0) == (U64)-24, 8);
    require(linux_service(Close, 0, 0, 0) == 0, 9);
    require(pidfd(self, 0x800) == 0, 10);
    require(linux_service(Close, 0, 0, 0) == 0, 11);
    require(linux_service(Close, 0, 0, 0) == (U64)-9, 12);
    finish(0);
  }
  U64 first = pidfd(self | (1UL << 32), 1UL << 32);
  require(first == 3, 13);
  U64 file = linux_service(OpenAt, (U64)-100, (U64) "/catalog/input", 0);
  require(file == 4, 14);
  unsigned char byte = 0x5a;
  require(linux_service(Read, first, (U64)&byte, 1) == (U64)-22, 15);
  require(byte == 0x5a, 16);
  require(linux_service(Read, first, ~0UL, ~0UL) == (U64)-22, 17);
  require(linux_service(Write, first, ~0UL, 1) == (U64)-22, 18);
  require(linux_service(Seek, first, 0, 0) == (U64)-29, 19);
  require(linux_service(WriteV, first, ~0UL, 0) == (U64)-22, 33);
  require(linux_service(WriteV, first, ~0UL, 1) == (U64)-14, 34);
  const struct {
    const void *base;
    U64 length;
  } vector = {&byte, 1};
  require(linux_service(WriteV, first, (U64)&vector, 1) == (U64)-22, 35);
  require(byte == 0x5a, 36);
  const struct {
    const void *base;
    U64 length;
  } invalid_vector = {&byte, ~0UL};
  require(linux_service(WriteV, first, (U64)&invalid_vector, 1) == (U64)-22,
          37);
  const struct {
    const void *base;
    U64 length;
  } invalid_range = {(void *)~0UL, 1};
  require(linux_service(WriteV, first, (U64)&invalid_range, 1) == (U64)-14, 38);
  const struct {
    const void *base;
    U64 length;
  } unmapped_payload = {(void *)1, 1};
  require(linux_service(WriteV, first, (U64)&unmapped_payload, 1) == (U64)-22,
          39);
  require(linux_service(WriteV, first, ~0UL, 1025) == (U64)-22, 40);
  require(linux_service(Seek, first, 0, 5) == (U64)-22, 41);
  require(pidfd(self, 1) == (U64)-22, 20);
  require(pidfd(self, 0) == (U64)-24, 21);
  require(linux_service(Close, first, 0, 0) == 0, 22);
  require(linux_service(Close, first, 0, 0) == (U64)-9, 23);
  require(linux_service(Read, first, (U64)&byte, 1) == (U64)-9, 24);
  require(linux_service(Write, first, (U64)&byte, 1) == (U64)-9, 25);
  U64 reused = linux_service(OpenAt, (U64)-100, (U64) "/catalog/input", 0);
  require(reused == first, 26);
  require(linux_service(Read, reused, (U64)&byte, 1) == 1 && byte == 0xa5, 27);
  require(linux_service(Read, file, (U64)&byte, 1) == 1 && byte == 0xa5, 28);
  require(linux_service(Close, file, 0, 0) == 0, 29);
  require(pidfd(self, 0x800) == 4, 30);
  require(linux_service(Close, 4, 0, 0) == 0, 31);
  require(linux_service(Close, reused, 0, 0) == 0, 32);
  finish(0);
}
