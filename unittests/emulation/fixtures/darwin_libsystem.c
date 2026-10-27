/* Independently authored SDK-free libSystem ABI workload. The GOT shims keep
 * ordinary classic imports non-lazy, and are also linked with chained fixups.
 * Function implementations are supplied only by the OS or the guest model. */
typedef unsigned long u64;
extern long read(int, void *, u64) __asm__("_probe_read");
extern long write(int, const void *, u64) __asm__("_probe_write");
extern int close(int) __asm__("_probe_close");
extern int getpid(void) __asm__("_probe_getpid");
extern int *__error(void) __asm__("_probe_error");
extern void _exit(int) __asm__("_probe_exit") __attribute__((noreturn));
#if defined(__aarch64__)
#define SHIM(Name, Symbol)                                                     \
  __asm__(".text\n.p2align 2\n.globl _probe_" Name "\n_probe_" Name            \
          ":\nadrp x16, " Symbol "@GOTPAGE\nldr x16, [x16, " Symbol            \
          "@GOTPAGEOFF]\nbr x16\n")
#define TAIL(Symbol)                                                           \
  __asm__("adrp x16, " Symbol "@GOTPAGE\nldr x16, [x16, " Symbol               \
          "@GOTPAGEOFF]\nbr x16")
#else
#define SHIM(Name, Symbol)                                                     \
  __asm__(".text\n.p2align 2\n.globl _probe_" Name "\n_probe_" Name            \
          ":\njmp *" Symbol "@GOTPCREL(%rip)\n")
#define TAIL(Symbol) __asm__("jmp *" Symbol "@GOTPCREL(%rip)")
#endif
SHIM("read", "_read");
SHIM("write", "_write");
SHIM("close", "_close");
SHIM("getpid", "_getpid");
SHIM("error", "___error");
SHIM("exit", "__exit");
__attribute__((naked, noinline)) static u64
wide_close(u64 fd __attribute__((unused))) {
  TAIL("_close");
}
__attribute__((naked, noinline)) static u64
wide_write(u64 fd __attribute__((unused)),
           const void *p __attribute__((unused)),
           u64 count __attribute__((unused))) {
  TAIL("_write");
}
static int sentinel = 0x234567;
static int *volatile relocated = &sentinel;
__attribute__((naked, noinline)) static int invalid_frame(void) {
#if defined(__aarch64__)
  __asm__("mov x30, xzr\nadrp x16, _getpid@GOTPAGE\n"
          "ldr x16, [x16, _getpid@GOTPAGEOFF]\nbr x16");
#else
  __asm__("movq $0, (%rsp)\njmp *_getpid@GOTPCREL(%rip)");
#endif
}
__attribute__((naked, noinline, noreturn)) static void terminal_frame(void) {
#if defined(__aarch64__)
  __asm__("mov x30, xzr\nmov x0, #0x123\nadrp x16, __exit@GOTPAGE\n"
          "ldr x16, [x16, __exit@GOTPAGEOFF]\nbr x16");
#else
  __asm__("movq $0, (%rsp)\nmovl $0x123, %edi\njmp *__exit@GOTPCREL(%rip)");
#endif
}
int main(int argc, char **argv) {
  if (argc == 2 && argv[1][0] == 'f')
    return invalid_frame();
  if (argc == 2 && argv[1][0] == 'n')
    terminal_frame();
  if (argc == 2 && argv[1][0] == 's') {
#if defined(__aarch64__)
    __asm__ volatile("svc #0x7f" ::: "memory");
#else
    register u64 number __asm__("rax") = 0;
    __asm__ volatile("syscall" : "+a"(number) : : "rcx", "r11", "cc", "memory");
#endif
    return 115;
  }
  if (argc == 2 && argv[1][0] == 'e') {
    if (write(1, "X", 1) != 1)
      return 101;
    _exit(0x123);
  }
  if (argc != 1 || relocated != &sentinel || *relocated != 0x234567)
    return 102;
  int *error = __error();
  if (*error || error != __error())
    return 103;
  *error = 71;
  int pid = getpid();
  if (pid <= 0 || pid != getpid() || *error != 71)
    return 104;
  const unsigned char bytes[] = {'A', 0, 255, 'B'};
  if (write(1, bytes, 4) != 4 || *error != 71)
    return 105;
  *error = 73;
  if (write(-1, bytes, 4) != -1 || *error != 9)
    return 106;
  unsigned char guard[] = {'x', 'y', 'z', 'q', 'r'};
  if (read(-1, guard + 1, 3) != -1 || *error != 9 || guard[1] != 'y')
    return 107;
  if (write(1, (void *)0, 1) != -1 || *error != 14)
    return 108;
  if (read(0, guard + 1, 3) != 3 || *error != 14 || guard[0] != 'x' ||
      guard[1] != 'a' || guard[2] != 'b' || guard[3] != 0 || guard[4] != 'r')
    return 109;
  if (close(-1) != -1 || *error != 9 || close(0) || *error != 9)
    return 110;
  if (read(0, guard, 0) != -1 || *error != 9 || guard[0] != 'x')
    return 111;
  if (wide_write(0x1234567800000001UL, "!", 1) != 1 || *error != 9)
    return 112;
  if (wide_close(0x12345678ffffffffUL) != ~0UL || *error != 9)
    return 113;
  return 58;
}
