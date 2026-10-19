//===- ModuleCAPISmoke.c - Compile/link the umbrella as pure C
//-------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/sdk/NeverDCAPI.h"

int neverd_test_module_c_header(void) {
  neverd_module_set_options_v1 Options = {sizeof(Options), "{}", NULL};
  const char *Error = NULL;
  neverd_module_set_t Set = neverd_module_set_create_v1(&Options, &Error);
  if (!Set) {
    neverd_free_string(Error);
    return 1;
  }
  const char *Report = neverd_module_set_inspect_json(Set, &Error);
  if (!Report) {
    neverd_free_string(Error);
    neverd_module_set_destroy(Set, NULL);
    return 2;
  }
  neverd_free_string(Report);
  if (!neverd_module_set_destroy(Set, &Error)) {
    neverd_free_string(Error);
    return 3;
  }
  return 0;
}
