//===- darwin_callback_leaf.c - Original startup/TLS workload
//---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file SDK-free dependency order, startup ABI and TLS callback controls.
#include "darwin_callback_startup.h"
int Order, Errors;
__thread int LeafTLS = 7;
int leaf_value(void) { return LeafTLS; }
void routine(int A, char **V, char **E, char **Apple,
             struct ProgramVars *Vars) {
  startup(A, V, E, Apple, Vars);
  if (Order)
    Errors += 8;
  Order = 9;
  write(1, "R", 1);
}
__attribute__((constructor)) static void
leaf_init(int A, char **V, char **E, char **Apple, struct ProgramVars *Vars) {
  startup(A, V, E, Apple, Vars);
  if (Order != 0 && Order != 9)
    Errors += 16;
  if (getpid() <= 0 || LeafTLS != 7)
    Errors += 32;
  LeafTLS = 19;
  Order = 1;
  write(1, "L", 1);
}
