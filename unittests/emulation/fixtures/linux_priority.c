//===- linux_priority.c - Independent raw priority ABI workloads ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned long U64;
extern U64 linux_service(U64, U64, U64, U64);
#if defined(__aarch64__)
enum { Get = 141, Set = 140, Exit = 93 };
#else
enum { Get = 140, Set = 141, Exit = 60 };
#endif
static void finish(U64 Status) {
  linux_service(Exit, Status, 0, 0);
  __builtin_trap();
}
static void check(U64 Actual, U64 Expected) {
  if (Actual != Expected)
    finish(91);
}
void process_main(U64 *Stack) {
  if (Stack[0] != 2)
    finish(91);
  char Mode = ((const char **)(Stack + 1))[1][0];
  if (Mode == 'm' || Mode == 'u' || Mode == 'g' || Mode == 's') {
    linux_service(Get,
                  Mode == 'g'   ? 1
                  : Mode == 's' ? 2
                                : 0,
                  Mode == 'u' ? 999 : 0, 0);
    finish(91);
  }
  check(linux_service(Get, 0, 0, 0), 20);
  check(linux_service(Set, 3, 0, 10), (U64)-22);
  check(linux_service(Get, 0, 0, 0), 20);
  if (Mode == 'c') {
    check(linux_service(Set, 0, 0, (U64)-100), 0);
    check(linux_service(Get, 0, 0, 0), 40);
    check(linux_service(Set, 0, 0, 0xdeadbeefffffffffUL), 0);
    check(linux_service(Get, 0, 0, 0), 21);
  } else if (Mode == 'l') {
    check(linux_service(Set, 0, 0, (U64)-10), 0);
    check(linux_service(Get, 0, 0, 0), 30);
    check(linux_service(Set, 0, 0, (U64)-11), (U64)-13);
    check(linux_service(Get, 0, 0, 0), 30);
  } else {
    check(linux_service(Set, 0, 0, 10), 0);
    check(linux_service(Get, 0, 1000, 0), 10);
    check(linux_service(Set, 0, 0, (U64)-20), (U64)-13);
    check(linux_service(Get, 0, 0, 0), 10);
    check(linux_service(Set, 0x1234567800000000UL, 0x12345678000003e8UL,
                        0x123456780000000aUL),
          0);
    check(linux_service(Set, 0, 0, 99), 0);
    check(linux_service(Get, 0, 0, 0), 1);
    check(linux_service(Get, 0, 1001, 0), 25);
    check(linux_service(Set, 0, 1001, 12), 0);
    check(linux_service(Get, 0, 1001, 0), 8);
    check(linux_service(Get, 0, 1000, 0), 1);
  }
  finish(0);
}
