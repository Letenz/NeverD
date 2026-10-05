/* Independently authored freestanding Darwin guest; no Apple SDK/libSystem. */
typedef unsigned long u64;
static volatile u64 data = 0x1234;
static volatile u64 bss;
static u64 secondary;

static u64 call(u64 number, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5,
                unsigned *error) {
#if defined(__aarch64__)
  register u64 x0 __asm__("x0") = a0;
  register u64 x1 __asm__("x1") = a1;
  register u64 x2 __asm__("x2") = a2;
  register u64 x3 __asm__("x3") = a3;
  register u64 x4 __asm__("x4") = a4;
  register u64 x5 __asm__("x5") = a5;
  register u64 x16 __asm__("x16") = number;
  unsigned carry;
  __asm__ volatile("svc #0x80\n\tcset %w2, cs"
                   : "+r"(x0), "+r"(x1), "=r"(carry)
                   : "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x16)
                   : "cc", "memory");
  *error = carry;
  secondary = x1;
  return x0;
#else
  register u64 r10 __asm__("r10") = a3;
  register u64 r8 __asm__("r8") = a4;
  register u64 r9 __asm__("r9") = a5;
  u64 result = number | 0x2000000;
  unsigned char carry;
  __asm__ volatile("syscall\n\tsetc %2"
                   : "+a"(result), "+d"(a2), "=qm"(carry)
                   : "D"(a0), "S"(a1), "r"(r10), "r"(r8), "r"(r9)
                   : "rcx", "r11", "cc", "memory");
  *error = carry;
  secondary = a2;
  return result;
#endif
}
static int equal(const char *a, const char *b) {
  while (*a && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}
static u64 little_integer(const unsigned char *p, unsigned width) {
  u64 value = 0;
  for (unsigned i = 0; i != width; ++i)
    value |= (u64)p[i] << (i * 8);
  return value;
}
/* Independent LP64 stat64 ABI checks; all three calls must describe the same
 * supplied regular file and must leave the open description's offset alone. */
static int file_status(const char *path) {
  unsigned error = 0;
  unsigned char a[146], b[146];
  int check = 80;
#define STATUS_EXPECT(expression)                                              \
  do {                                                                         \
    ++check;                                                                   \
    if (!(expression))                                                         \
      return check;                                                            \
  } while (0)
  a[0] = b[0] = 0xab;
  a[145] = b[145] = 0xcd;
  STATUS_EXPECT(call(338, (u64)path, (u64)(a + 1), 0, 0, 0, 0, &error) == 0 &&
                !error);
  STATUS_EXPECT(a[0] == 0xab && a[145] == 0xcd);
  STATUS_EXPECT((little_integer(a + 5, 2) & 0170000) == 0100000);
  STATUS_EXPECT(little_integer(a + 7, 2) != 0);
  STATUS_EXPECT(little_integer(a + 97, 8) == 10);
  STATUS_EXPECT(little_integer(a + 113, 4) > 0);
  for (unsigned i = 41; i <= 89; i += 16)
    STATUS_EXPECT(little_integer(a + i, 8) < 1000000000);
  for (unsigned i = 25; i != 33; ++i)
    STATUS_EXPECT(a[i] == 0);
  for (unsigned i = 125; i != 145; ++i)
    STATUS_EXPECT(a[i] == 0);
  u64 fd = call(5, (u64)path, 0, 0, 0, 0, 0, &error);
  STATUS_EXPECT(!error);
  STATUS_EXPECT(call(199, fd, 2, 0, 0, 0, 0, &error) == 2 && !error);
  u64 copy = call(41, fd, 0, 0, 0, 0, 0, &error);
  STATUS_EXPECT(!error && copy != fd);
  STATUS_EXPECT(call(6, fd, 0, 0, 0, 0, 0, &error) == 0 && !error);
  STATUS_EXPECT(call(339, fd, 0, 0, 0, 0, 0, &error) == 9 && error);
  STATUS_EXPECT(call(339, copy, (u64)(b + 1), 0, 0, 0, 0, &error) == 0 &&
                !error);
  for (unsigned i = 0; i != 146; ++i)
    if (a[i] != b[i])
      return 130;
  STATUS_EXPECT(call(199, copy, 0, 1, 0, 0, 0, &error) == 2 && !error);
  STATUS_EXPECT(call(340, (u64)path, (u64)(b + 1), 0, 0, 0, 0, &error) == 0 &&
                !error);
  for (unsigned i = 0; i != 146; ++i)
    if (a[i] != b[i])
      return 131;
  STATUS_EXPECT(call(339, copy, 0, 0, 0, 0, 0, &error) == 14 && error);
  STATUS_EXPECT(call(338, (u64)path, 0, 0, 0, 0, 0, &error) == 14 && error);
  STATUS_EXPECT(call(340, (u64) "", 0, 0, 0, 0, 0, &error) == 2 && error);
  char descendant[1024];
  unsigned length = 0;
  while (path[length] && length < 700) {
    descendant[length] = path[length];
    ++length;
  }
  STATUS_EXPECT(!path[length]);
  descendant[length] = '/';
  for (unsigned i = 0; i != 256; ++i)
    descendant[length + 1 + i] = 'a';
  descendant[length + 257] = 0;
  STATUS_EXPECT(call(338, (u64)descendant, 0, 0, 0, 0, 0, &error) == 20 &&
                error);
  descendant[length] = '-';
  descendant[length + 1] = 'x';
  descendant[length + 2] = 0;
  STATUS_EXPECT(call(340, (u64)descendant, 0, 0, 0, 0, 0, &error) == 2 &&
                error);
  STATUS_EXPECT(call(6, copy, 0, 0, 0, 0, 0, &error) == 0 && !error);
  STATUS_EXPECT(call(4, 1, (u64) "s", 1, 0, 0, 0, &error) == 1 && !error);
#undef STATUS_EXPECT
  return 37;
}
#if defined(__aarch64__)
#define PAGE 16384UL
#else
#define PAGE 4096UL
#endif
/* The same raw-call object runs in the guest and against the native kernel.
 * The harness supplies a regular file containing exactly 0123456789. */
static int file_calls(const char *path, unsigned nocancel) {
  const u64 open_call = nocancel ? 398 : 5;
  const u64 read_call = nocancel ? 396 : 3;
  const u64 pread_call = nocancel ? 414 : 153;
  const u64 close_call = nocancel ? 399 : 6;
  const u64 fcntl_call = nocancel ? 406 : 92;
  unsigned error = 0;
  unsigned char bytes[8];
  int check = 140;
#define FILE_EXPECT(expression)                                                \
  do {                                                                         \
    ++check;                                                                   \
    if (!(expression))                                                         \
      return check;                                                            \
  } while (0)
  u64 fd = call(open_call, (u64)path, 0x1000000, 0, 0, 0, 0, &error);
  FILE_EXPECT(!error);
  char descendant[1024];
  unsigned length = 0;
  while (path[length] && length < 700) {
    descendant[length] = path[length];
    ++length;
  }
  FILE_EXPECT(!path[length]);
  descendant[length] = '/';
  for (unsigned i = 0; i != 256; ++i)
    descendant[length + 1 + i] = 'a';
  descendant[length + 257] = 0;
  FILE_EXPECT(call(open_call, (u64)descendant, 0, 0, 0, 0, 0, &error) == 20 &&
              error);
  const char missing[] = "-missing/";
  for (unsigned i = 0; i != 9; ++i)
    descendant[length + i] = missing[i];
  for (unsigned i = 0; i != 256; ++i)
    descendant[length + 9 + i] = 'a';
  descendant[length + 265] = 0;
  FILE_EXPECT(call(open_call, (u64)descendant, 0, 0, 0, 0, 0, &error) == 2 &&
              error);
  FILE_EXPECT(call(fcntl_call, fd, 1, 0, 0, 0, 0, &error) == 1 && !error);
  FILE_EXPECT(call(fcntl_call, fd, 3, 0, 0, 0, 0, &error) == 0 && !error);
  FILE_EXPECT(call(read_call, fd, (u64)bytes, 2, 0, 0, 0, &error) == 2 &&
              !error && bytes[0] == '0' && bytes[1] == '1');
  u64 copy = call(41, fd, 0, 0, 0, 0, 0, &error);
  FILE_EXPECT(!error && copy != fd);
  FILE_EXPECT(call(fcntl_call, copy, 1, 0, 0, 0, 0, &error) == 0 && !error);
  FILE_EXPECT(call(read_call, copy, (u64)bytes, 2, 0, 0, 0, &error) == 2 &&
              !error && bytes[0] == '2' && bytes[1] == '3');
  FILE_EXPECT(call(199, fd, 0, 1, 0, 0, 0, &error) == 4 && !error);
  FILE_EXPECT(call(pread_call, copy, (u64)bytes, 4, 8, 0, 0, &error) == 2 &&
              !error && bytes[0] == '8' && bytes[1] == '9');
  FILE_EXPECT(call(199, fd, 0, 1, 0, 0, 0, &error) == 4 && !error);
  u64 other = call(open_call, (u64)path, 0, 0, 0, 0, 0, &error);
  FILE_EXPECT(!error && other != fd && other != copy);
  FILE_EXPECT(call(read_call, other, (u64)bytes, 1, 0, 0, 0, &error) == 1 &&
              !error && bytes[0] == '0');
  FILE_EXPECT(call(90, fd, other, 0, 0, 0, 0, &error) == other && !error);
  FILE_EXPECT(call(read_call, other, (u64)bytes, 1, 0, 0, 0, &error) == 1 &&
              !error && bytes[0] == '4');
  FILE_EXPECT(call(close_call, fd, 0, 0, 0, 0, 0, &error) == 0 && !error);
  FILE_EXPECT(call(read_call, fd, (u64)bytes, 1, 0, 0, 0, &error) == 9 &&
              error);
  FILE_EXPECT(call(read_call, other, (u64)bytes, 1, 0, 0, 0, &error) == 1 &&
              !error && bytes[0] == '5');
  FILE_EXPECT(call(fcntl_call, other, 2, 3, 0, 0, 0, &error) == 0 && !error);
  FILE_EXPECT(call(90, other, other, 0, 0, 0, 0, &error) == other && !error);
  FILE_EXPECT(call(fcntl_call, other, 1, 0, 0, 0, 0, &error) == 1 && !error);
  FILE_EXPECT(call(fcntl_call, other, 67, 15, 0, 0, 0, &error) == 15 && !error);
  FILE_EXPECT(call(fcntl_call, 15, 1, 0, 0, 0, 0, &error) == 1 && !error);
  FILE_EXPECT(call(fcntl_call, other, 0, 15, 0, 0, 0, &error) == 16 && !error);
  FILE_EXPECT(call(fcntl_call, 16, 1, 0, 0, 0, 0, &error) == 0 && !error);
  FILE_EXPECT(call(read_call, 16, (u64)bytes, 1, 0, 0, 0, &error) == 1 &&
              !error && bytes[0] == '6');
  FILE_EXPECT(call(199, 15, 0, 1, 0, 0, 0, &error) == 7 && !error);
  FILE_EXPECT(call(199, 15, (u64)-8, 1, 0, 0, 0, &error) == 22 && error);
  FILE_EXPECT(call(199, 15, 0, 1, 0, 0, 0, &error) == 7 && !error);
  FILE_EXPECT(call(199, 15, 0x7fffffffffffffffUL, 0, 0, 0, 0, &error) ==
                  0x7fffffffffffffffUL &&
              !error);
  FILE_EXPECT(call(199, 15, 1, 1, 0, 0, 0, &error) == 84 && error);
  FILE_EXPECT(call(pread_call, other, 0, 0, (u64)-1, 0, 0, &error) == 22 &&
              error);
  FILE_EXPECT(call(read_call, other, 0, 1, 0, 0, 0, &error) == 0 && !error);
  FILE_EXPECT(call(read_call, 99, 0, 0x80000000UL, 0, 0, 0, &error) == 22 &&
              error);
  FILE_EXPECT(call(read_call, 99, 0, 1, 0, 0, 0, &error) == 9 && error);
  FILE_EXPECT(call(199, 15, 0, 0, 0, 0, 0, &error) == 0 && !error);
  FILE_EXPECT(call(read_call, other, 0, 1, 0, 0, 0, &error) == 14 && error);
  FILE_EXPECT(call(199, 15, 0, 1, 0, 0, 0, &error) == 0 && !error);
  FILE_EXPECT(call(read_call, other | (1UL << 32), (u64)bytes, 1, 0, 0, 0,
                   &error) == 1 &&
              !error && bytes[0] == '0');
  FILE_EXPECT(call(close_call, other, 0, 0, 0, 0, 0, &error) == 0 && !error);
  FILE_EXPECT(call(close_call, other, 0, 0, 0, 0, 0, &error) == 9 && error);
  FILE_EXPECT(call(close_call, copy, 0, 0, 0, 0, 0, &error) == 0 && !error);
  FILE_EXPECT(call(close_call, 15, 0, 0, 0, 0, 0, &error) == 0 && !error);
  FILE_EXPECT(call(close_call, 16, 0, 0, 0, 0, 0, &error) == 0 && !error);
  bytes[0] = 'f';
  FILE_EXPECT(call(nocancel ? 397 : 4, 1, (u64)bytes, 1, 0, 0, 0, &error) ==
                  1 &&
              !error);
#undef FILE_EXPECT
  return 37;
}

static int output_descriptors(void) {
  unsigned error;
  const char text[] = "ok";
  u64 fd = call(41, 1, 0, 0, 0, 0, 0, &error);
  if (error || call(6, 1, 0, 0, 0, 0, 0, &error) || error)
    return 201;
  if (call(4, 1, (u64)text, 1, 0, 0, 0, &error) != 9 || !error)
    return 202;
  if (call(90, fd, 2, 0, 0, 0, 0, &error) != 2 || error ||
      call(4, 2, (u64)text, 1, 0, 0, 0, &error) != 1 || error)
    return 203;
  if (call(6, fd, 0, 0, 0, 0, 0, &error) || error ||
      call(90, 2, 1, 0, 0, 0, 0, &error) != 1 || error ||
      call(397, 1, (u64)(text + 1), 1, 0, 0, 0, &error) != 1 || error)
    return 204;
  return 37;
}
static volatile unsigned char reclaimable[PAGE * 2]
    __attribute__((aligned(PAGE)));
#ifdef DARWIN_THREAD_ENTRY
#define main darwin_body
#endif
int main(int argc, char **argv, char **envp, char **apple) {
  unsigned error = 0;
  if (argc < 2 || data != 0x1234 || bss != 0)
    return 101;
  bss = 99;
  if (equal(argv[1], "files") || equal(argv[1], "files-nocancel"))
    return argc < 3 ? 139
                    : file_calls(argv[2], equal(argv[1], "files-nocancel"));
  if (equal(argv[1], "file-status"))
    return argc < 3 ? 132 : file_status(argv[2]);
  if (equal(argv[1], "output-descriptors"))
    return output_descriptors();
  if (equal(argv[1], "stdin")) {
    unsigned char bytes[4];
    u64 fd = call(41, 0, 0, 0, 0, 0, 0, &error);
    if (error || call(3, fd, (u64)bytes, 2, 0, 0, 0, &error) != 2 || error ||
        bytes[0] != 0 || bytes[1] != 255)
      return 205;
    if (call(396, 0, (u64)(bytes + 2), 2, 0, 0, 0, &error) != 1 || error ||
        bytes[2] != 'x' || call(3, fd, 0, 1, 0, 0, 0, &error) || error)
      return 206;
    if (call(4, 1, (u64)bytes, 3, 0, 0, 0, &error) != 3 || error)
      return 207;
    return 37;
  }
  if (equal(argv[1], "loop"))
    for (;;)
      ++bss;
  if (equal(argv[1], "fault"))
    *(volatile u64 *)0 = 1;
  if (equal(argv[1], "unknown"))
    return (int)call(999, 0, 0, 0, 0, 0, 0, &error);
  if (equal(argv[1], "mach"))
    return (int)call((u64)-31, 0, 0, 0, 0, 0, 0, &error);
  if (equal(argv[1], "badtrap")) {
#if defined(__aarch64__)
    __asm__ volatile("svc #0" ::: "memory");
#else
    __asm__ volatile("mov $1, %%eax\n\tsyscall" ::
                         : "rax", "rcx", "r11", "memory");
#endif
  }
  if (equal(argv[1], "exit"))
    return (int)call(1, 37, 0, 0, 0, 0, 0, &error);
  if (equal(argv[1], "return"))
    return 37;
  if (equal(argv[1], "write-length")) {
    const u64 counts[] = {0x80000000UL, (u64)-1};
    for (unsigned i = 0; i < 2; ++i)
      for (unsigned j = 0; j < 2; ++j)
        if (call(4, j ? 99 : 1, 0, counts[i], 0, 0, 0, &error) != 22 || !error)
          return 127;
    if (call(4, 99, 0, 0x7fffffffUL, 0, 0, 0, &error) != 9 || !error)
      return 128;
    if (call(4, 1, 0, 1, 0, 0, 0, &error) != 14 || !error)
      return 129;
    const char byte = 'w';
    if (call(4, 1, (u64)&byte, 1, 0, 0, 0, &error) != 1 || error)
      return 130;
    return 37;
  }
  if (equal(argv[1], "release")) {
    u64 sp;
#if defined(__aarch64__)
    __asm__ volatile("mov %0, sp" : "=r"(sp));
#else
    __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
#endif
    const u64 pages[] = {(sp & ~(PAGE - 1)) - PAGE, (u64)&reclaimable[PAGE]};
    for (unsigned i = 0; i < 2; ++i) {
      if (call(197, 0, PAGE, 3, 0x1002, (u64)-1, 0, &error) != 12 || !error)
        return 124;
      if (call(73, pages[i], PAGE, 0, 0, 0, 0, &error) || error)
        return 125;
      u64 fresh = call(197, 0, PAGE, 3, 0x1002, (u64)-1, 0, &error);
      if (error || *(volatile u64 *)fresh)
        return 126;
    }
    return 37;
  }
  if (equal(argv[1], "identity")) {
    const u64 numbers[] = {24, 25, 39, 43, 47};
    for (unsigned i = 0; i < 5; ++i)
      if (call(numbers[i], 0, 0, 7, 0, 0, 0, &error) != (i == 2 ? 1 : 1000) ||
          error || secondary)
        return 120;
    return 37;
  }
  if (equal(argv[1], "partial")) {
    u64 address = call(197, 0, PAGE, 3, 0x1002, (u64)-1, 0, &error);
    if (error)
      return 121;
    volatile unsigned char *end = (void *)(address + PAGE - 2);
    end[0] = 'p';
    end[1] = 'q';
    if (call(4, 1, (u64)end, 4, 0, 0, 0, &error) != 14 || !error)
      return 122;
    if (call(4, 1, (u64)end, 1, 0, 0, 0, &error) != 1 || error)
      return 123;
    return 37;
  }
  if (equal(argv[1], "memory") || equal(argv[1], "permission")) {
    u64 address = call(197, 0, PAGE * 3, 3, 0x1002, (u64)-1, 0, &error);
    if (error || address % PAGE)
      return 110;
    volatile unsigned char *memory = (void *)address;
    memory[0] = 'a';
    memory[PAGE] = 'b';
    memory[PAGE * 2] = 'c';
    if (equal(argv[1], "permission")) {
      if (call(74, address, PAGE, 1, 0, 0, 0, &error) || error)
        return 111;
      memory[0] = 'x';
      return 112;
    }
    if (call(73, address + 1, PAGE, 0, 0, 0, 0, &error) != 22 || !error)
      return 113;
    if (call(73, address + PAGE, PAGE, 0, 0, 0, 0, &error) || error)
      return 114;
    if (call(74, address, PAGE * 3, 0, 0, 0, 0, &error) != 12 || !error)
      return 115;
    memory[0] = 'd'; /* Failed protect across a hole must preserve this page. */
    if (call(4, 1, address, 1, 0, 0, 0, &error) != 1 || error)
      return 116;
    if (call(4, 1, address + PAGE, 1, 0, 0, 0, &error) != 14 || !error)
      return 117;
    if (call(73, address, PAGE * 3, 0, 0, 0, 0, &error) || error)
      return 118;
    u64 reused = call(197, address, PAGE * 3, 2, 0x1002, (u64)-1, 0, &error);
    if (error || reused != address || *(volatile u64 *)reused != 0)
      return 119;
    return 37;
  }
  if (argc != 3 || !equal(argv[0], "guest") || !equal(argv[2], "argument") ||
      argv[3] || !envp[0] || !equal(envp[0], "MODE=test") || envp[1] ||
      !apple[0] || apple[1])
    return 102;
  const char prefix[] = "executable_path=";
  for (unsigned i = 0; i < sizeof(prefix) - 1; ++i)
    if (apple[0][i] != prefix[i])
      return 103;
  if (call(4, 99, 0, 7, 0, 0, 0, &error) != 9 || !error)
    return 104;
#if defined(__aarch64__)
  if (secondary != 0)
    return 105;
#else
  if (secondary != 7)
    return 105;
#endif
  if (call(20, 0, 0, 7, 0, 0, 0, &error) != 1000 || error || secondary)
    return 106;
  if (call(4, 1, 0, 1, 0, 0, 0, &error) != 14 || !error)
    return 107;
  const unsigned char bytes[] = {'d', 'a', 'r', 'w', 'i', 'n', 0, 255, '\n'};
  if (call(4, 1, (u64)bytes, sizeof(bytes), 0, 0, 0, &error) != sizeof(bytes) ||
      error)
    return 108;
  return 37;
}
