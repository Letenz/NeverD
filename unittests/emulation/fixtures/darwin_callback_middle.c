//===- darwin_callback_middle.c - Original startup/TLS workload
//---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file SDK-free dependency order, startup ABI and TLS callback controls.
#include "darwin_callback_startup.h"
extern int leaf_value(void);
int middle_value(void) { return leaf_value(); }
__attribute__((constructor)) static void
parent_init(int A, char **V, char **E, char **Apple, struct ProgramVars *Vars) {
  startup(A, V, E, Apple, Vars);
  if (Order != 1 || leaf_value() != 19)
    Errors += 64;
  Order = 12;
  write(1, "P", 1);
}
