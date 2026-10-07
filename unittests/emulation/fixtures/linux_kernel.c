//===- linux_kernel.c - Independent absent-system-call workloads ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned long U64;
extern U64 linux_service(U64, U64, U64, U64);
#if defined(__aarch64__)
enum { GetPID = 172, Exit = 93 };
#else
enum { GetPID = 39, Exit = 60 };
#endif
static void finish(U64 status) {
  linux_service(Exit, status, 0, 0);
  __builtin_trap();
}
void process_main(U64 *stack) {
  if (stack[0] != 2)
    finish(91);
  char mode = ((const char **)(stack + 1))[1][0];
  if (mode == 'x') {
    linux_service(999, 0, 0, 0);
    finish(91);
  }
  U64 r = linux_service(434, linux_service(GetPID, 0, 0, 0), 0, 0);
  if (mode != 'u' || r != (U64)-38)
    finish(91);
  /* An absent call has no argument validation or output side effects. */
  r = linux_service(434, ~0UL, ~0UL, ~0UL);
  if (r != (U64)-38)
    finish(91);
  finish(0);
}
