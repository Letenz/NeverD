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
#if defined(__aarch64__)
#define PAGE 16384UL
#else
#define PAGE 4096UL
#endif
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
