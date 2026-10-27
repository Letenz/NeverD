//===- darwin_callback_main.cpp - Original startup/TLS workload
//---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file SDK-free dependency order, startup ABI and TLS callback controls.
extern "C" {
#include "darwin_callback_startup.h"
extern int middle_value(void);
extern int leaf_value(void);
extern void _tlv_atexit(void (*)(void *), void *);
extern void exit(int) __attribute__((noreturn));
extern void _exit(int) __attribute__((noreturn));
}
static const char *ExitMode;
static void nested(void *Object) {
  if ((U64)Object != 81)
    _exit(97);
  write(1, "N", 1);
}
struct Local {
  int Value;
  char Name;
  Local(int V, char N) : Value(V), Name(N) { write(1, &Name, 1); }
  ~Local() {
    char N = Name == 'a' ? '1' : '2';
    if (Value != (Name == 'a' ? 33 : 43))
      _exit(96);
    write(1, &N, 1);
    if (Name == 'b' && equal(ExitMode, "dtor-raw"))
      _exit(39);
    if (Name == 'b' && equal(ExitMode, "dtor-exit"))
      exit(40);
    if (Name == 'b')
      _tlv_atexit(nested, (void *)81);
  }
};
thread_local Local First(31, 'a');
thread_local Local Second(41, 'b');
__attribute__((constructor(101))) static void
main_first(int A, char **V, char **E, char **Apple, ProgramVars *Vars) {
  startup(A, V, E, Apple, Vars);
  ExitMode = V[1];
  if (Order != 12)
    Errors += 512;
  Order = 123;
  write(1, "E", 1);
  if (First.Value != 31 || Second.Value != 41)
    Errors += 1024;
  First.Value = 33;
  Second.Value = 43;
  if (Errors)
    _exit(99);
  if (equal(V[1], "ctor-exit"))
    exit(37);
  if (equal(V[1], "ctor-raw"))
    _exit(38);
}
__attribute__((constructor(201))) static void
main_second(int A, char **V, char **E, char **Apple, ProgramVars *Vars) {
  startup(A, V, E, Apple, Vars);
  if (Order != 123 || First.Value != 33 || Second.Value != 43)
    Errors += 2048;
  Order = 1234;
  write(1, "F", 1);
}
struct StackCanary {
  U64 Word[8];
};
static void stack_cleanup(void *Object) {
  volatile StackCanary *Canary = (volatile StackCanary *)Object;
  for (unsigned I = 0; I < 8; ++I)
    if (Canary->Word[I] != 0x1122334455667700UL + I)
      _exit(95);
  write(1, "S", 1);
}
__attribute__((noinline)) static void deep_exit() {
  volatile U64 Pad[32];
  for (unsigned I = 0; I < 32; ++I)
    Pad[I] = I + 33;
  __asm__ volatile("" : : "r"(Pad) : "memory");
  exit(37);
}
__attribute__((noinline)) static void exit_with_stack_object() {
  volatile StackCanary Canary;
  for (unsigned I = 0; I < 8; ++I)
    Canary.Word[I] = 0x1122334455667700UL + I;
  _tlv_atexit(stack_cleanup, (void *)&Canary);
  deep_exit();
}
extern "C" int main(int A, char **V) {
  if (A != 2 || Order != 1234 || middle_value() != 19 || leaf_value() != 19 ||
      First.Value != 33 || Second.Value != 43 || &First == &Second || Errors)
    return 99;
  write(1, "M", 1);
  if (equal(V[1], "exit"))
    exit(37);
  if (equal(V[1], "stack-exit"))
    exit_with_stack_object();
  if (equal(V[1], "raw"))
    _exit(38);
  return 37;
}
