//===- linux_signals.c - Independent x64/ARM64 signal action syscalls ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned long U64;
extern U64 linux_service(U64, U64, U64, U64);
struct Action {
  U64 Handler, Flags, Restorer, Mask;
};
static struct Action Old, New = {1, 0x10000000, 0x123456789abcdef0UL, ~0UL};
#if defined(__aarch64__)
enum { Write = 64, Exit = 93 };
static U64 action(U64 Signal, U64 Input, U64 Output, U64 Size) {
  register U64 X0 __asm__("x0") = Signal;
  register U64 X1 __asm__("x1") = Input;
  register U64 X2 __asm__("x2") = Output;
  register U64 X3 __asm__("x3") = Size;
  register U64 X8 __asm__("x8") = 134;
  __asm__ volatile("svc #0"
                   : "+r"(X0)
                   : "r"(X1), "r"(X2), "r"(X3), "r"(X8)
                   : "memory");
  return X0;
}
#else
enum { Write = 1, Exit = 60 };
static U64 action(U64 Signal, U64 Input, U64 Output, U64 Size) {
  register U64 R10 __asm__("r10") = Size;
  U64 Result;
  __asm__ volatile("syscall"
                   : "=a"(Result)
                   : "a"(13UL), "D"(Signal), "S"(Input), "d"(Output), "r"(R10)
                   : "rcx", "r11", "memory");
  return Result;
}
#endif
static void finish(U64 Status) {
  linux_service(Exit, Status, 0, 0);
  __builtin_trap();
}
static void check(U64 Value, U64 Expected) {
  if (Value != Expected)
    finish(91);
}
static void output(void) {
  check(linux_service(Write, 1, (U64)&Old, sizeof(Old)), sizeof(Old));
}
void process_main(U64 *Stack) {
  if (Stack[0] != 2)
    finish(91);
  char Mode = ((const char **)(Stack + 1))[1][0];
  if (Mode == 'm') {
    action(11, 0, (U64)&Old, 8);
    finish(91);
  }
  check(action(0, 1, 1, 7), (U64)-22);
  check(action(0, 1, 1, 8), (U64)-14);
  check(action(0, 0, 1, 8), (U64)-22);
  check(action(9, (U64)&New, 0, 8), (U64)-22);
  check(action(0x123456780000000bUL, 0, (U64)&Old, 8), 0);
  output();
  check(action(11, (U64)&New, 1, 8), (U64)-14);
  check(action(11, 0, (U64)&Old, 8), 0);
  check(Old.Handler, New.Handler);
  check(Old.Flags, New.Flags);
  check(Old.Restorer, New.Restorer);
  check(Old.Mask, 0xfffffffffffbfeffUL);
  output();
  check(action(64, (U64)&New, 0, 8), 0);
  check(action(64, 0, (U64)&Old, 8), 0);
  output();
  finish(0);
}
