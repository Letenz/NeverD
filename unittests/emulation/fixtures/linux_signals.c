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
enum {
  Write = 64,
  Exit = 93,
  Kill = 129,
  GetPID = 172,
  SigAction = 134,
  SigMask = 135,
  SigPending = 136
};
static U64 call4(U64 Number, U64 Signal, U64 Input, U64 Output, U64 Size) {
  register U64 X0 __asm__("x0") = Signal;
  register U64 X1 __asm__("x1") = Input;
  register U64 X2 __asm__("x2") = Output;
  register U64 X3 __asm__("x3") = Size;
  register U64 X8 __asm__("x8") = Number;
  __asm__ volatile("svc #0"
                   : "+r"(X0)
                   : "r"(X1), "r"(X2), "r"(X3), "r"(X8)
                   : "memory");
  return X0;
}
#else
enum {
  Write = 1,
  Exit = 60,
  Kill = 62,
  GetPID = 39,
  SigAction = 13,
  SigMask = 14,
  SigPending = 127
};
static U64 call4(U64 Number, U64 Signal, U64 Input, U64 Output, U64 Size) {
  register U64 R10 __asm__("r10") = Size;
  U64 Result;
  __asm__ volatile("syscall"
                   : "=a"(Result)
                   : "a"(Number), "D"(Signal), "S"(Input), "d"(Output), "r"(R10)
                   : "rcx", "r11", "memory");
  return Result;
}
#endif
static U64 action(U64 Signal, U64 Input, U64 Output, U64 Size) {
  return call4(SigAction, Signal, Input, Output, Size);
}
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
static volatile U64 Count, HandlerMask;
static void handler(int Signal) {
  Count += (unsigned)Signal;
  call4(SigMask, 0, 0, (U64)&HandlerMask, 8);
}
void process_main(U64 *Stack) {
  if (Stack[0] != 2)
    finish(91);
  char Mode = ((const char **)(Stack + 1))[1][0];
  if (Mode == 'v' || Mode == 'd' || Mode == 's') {
    U64 Set = (1UL << 9) | (1UL << 8) | (1UL << 18);
    static U64 Values[8];
    U64 PID = linux_service(GetPID, 0, 0, 0);
    struct Action A = {1, 0, 0, 0};
    if (Mode == 'd') {
      A.Handler = 0;
      check(action(15, (U64)&A, 0, 8), 0);
      linux_service(Kill, PID, 15, 0);
      finish(91);
    }
    if (Mode == 's') {
      A.Handler = (U64)handler;
      check(action(10, (U64)&A, 0, 8), 0);
      check(linux_service(Kill, PID, 10, 0), 0);
      Values[0] = Count;
      Values[1] = HandlerMask;
      check(call4(SigMask, 0, 0, (U64)(Values + 2), 8), 0);
    } else {
      check(action(10, (U64)&A, 0, 8), 0);
      check(
          call4(SigMask, 0x1234567800000002UL, (U64)&Set, (U64)(Values + 0), 8),
          0);
      check(call4(SigMask, 0, 0, (U64)(Values + 1), 8), 0);
      check(linux_service(Kill, PID, 10, 0), 0);
      check(linux_service(Kill, PID, 10, 0), 0);
      check(linux_service(SigPending, (U64)(Values + 2), 8, 0), 0);
      // Installing ignore again discards the blocked pending instance.
      check(action(10, (U64)&A, 0, 8), 0);
      check(linux_service(SigPending, (U64)(Values + 3), 8, 0), 0);
      Set = 0;
      check(call4(SigMask, 2, (U64)&Set, 0, 8), 0);
      check(linux_service(SigPending, 1, 0, 0), 0);
      check(linux_service(SigPending, 1, 9, 0), (U64)-22);
      check(call4(SigMask, 2, 1, 0, 7), (U64)-22);
      check(call4(SigMask, 2, 1, 0, 8), (U64)-14);
      Set = 1UL << 12;
      check(call4(SigMask, 2, (U64)&Set, 1, 8), (U64)-14);
      check(call4(SigMask, 0, 0, (U64)(Values + 4), 8), 0);
    }
    check(linux_service(Write, 1, (U64)Values, sizeof(Values)), sizeof(Values));
    finish(0);
  }
  if (Mode == 'k' || Mode == 'q' || Mode == 'u' || Mode == 'h') {
    U64 PID = linux_service(GetPID, 0, 0, 0);
    check(linux_service(Kill, PID, 65, 0), (U64)-22);
    check(linux_service(Kill, PID, ~0UL, 0), (U64)-22);
    check(linux_service(Kill, PID | 0x1234567800000000UL, 0x1234567800000000UL,
                        0),
          0);
    if (Mode == 'q')
      finish(0);
    linux_service(Write, 1, (U64) "before", 6);
    linux_service(Kill, Mode == 'u' ? 0 : PID, Mode == 'h' ? 15 : 9, 0);
    linux_service(Write, 1, (U64) "after", 5);
    finish(91);
  }
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
