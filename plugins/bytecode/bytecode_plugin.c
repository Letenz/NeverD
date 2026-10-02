//===- bytecode_plugin.c - External bytecode rules in a C plugin -*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/sdk/NeverDPlugin.h"

#include <stdio.h>
#include <string.h>

static int recover(neverd_session_t Session, int Arg) {
  (void)Session;
  (void)Arg;
  // Independently constructed example. A real plugin reads its own container
  // and supplies its encoding rules, function boundaries and call bindings.
  const unsigned char Code[] = {0x6d};
  const char Request[] =
      "{\"schemaVersion\":1,\"output\":\"highc\","
      "\"functions\":[{\"entry\":0,\"end\":1,\"name\":\"plugin_return\"}],"
      "\"profile\":{\"version\":1,\"register_bytes\":8,\"byte_order\":"
      "\"little\","
      "\"encodings\":[{\"size\":1,\"match\":[{\"offset\":0,\"value\":109}],"
      "\"operations\":[{\"op\":\"RETURN\",\"inputs\":[]}]}]}}";
  const char *Report = neverd_bytecode_recover_json_v1(
      Code, sizeof(Code), Request, sizeof(Request) - 1);
  if (!Report)
    return 1;
  // The C API emits compact canonical JSON. Applications needing individual
  // fields should parse the report with their existing JSON library.
  int Failed = strstr(Report, "\"ok\":true") == NULL;
  puts(Report);
  neverd_free_string(Report);
  return Failed;
}

NEVERD_PLUGIN_EXPORT neverd_plugin_t neverd_plugin = {
    .Name = "External Bytecode C",
    .Version = "1.0.0",
    .Author = "NeverD contributors",
    .Description = "Supplies external bytecode rules through the public C ABI",
    .Type = NEVERD_PLUGIN_PROCESSOR,
    .Run = recover,
};
