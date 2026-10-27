//===- ExecutionCAPISmoke.c - Pure C persistent execution consumer
//---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/sdk/NeverDCAPI.h"

#include <string.h>

int neverd_test_execution_c_header(void) {
  const char *Error = NULL;
  neverd_module_set_t Sources = neverd_module_set_create_v1(NULL, &Error);
  if (!Sources) {
    neverd_free_string(Error);
    return 0;
  }
  neverd_execution_options_v1 Options = {
      sizeof(Options), Sources,
      "{\"environment\":{\"kind\":\"buffer\","
      "\"cpu\":{\"backend\":\"unicorn\"},"
      "\"entry\":65536,\"stop_pc\":65537,\"stack_pointer\":528320,"
      "\"regions\":[{\"address\":65536,\"size\":4096,"
      "\"permissions\":\"rx\",\"bytes_hex\":\"9090\"}]}}"};
  neverd_execution_t Run = neverd_execution_create_v1(&Options, &Error);
  neverd_module_set_destroy(Sources, NULL);
  if (!Run) {
    neverd_free_string(Error);
    return 0;
  }
  const char *Event = neverd_execution_run_json(Run, &Error);
  int Passed = Event && strstr(Event, "completed") != NULL && Error == NULL;
  neverd_free_string(Event);
  neverd_free_string(Error);
  return neverd_execution_destroy(Run, NULL) && Passed;
}
