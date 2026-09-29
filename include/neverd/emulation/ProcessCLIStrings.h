//===- ProcessCLIStrings.h - Process command-line vocabulary ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_PROCESSCLISTRINGS_H
#define NEVERD_EMULATION_PROCESSCLISTRINGS_H
namespace neverd::process_cli {
#define NEVERD_PROCESS_CLI_STRING(Name, Text)                                  \
  inline constexpr char Name[] = Text;
#define NEVERD_PROCESS_CLI_STATUS(Name, Value)                                 \
  inline constexpr int Name = Value;
#include "neverd/emulation/ProcessReport.def"
#undef NEVERD_PROCESS_CLI_STATUS
#undef NEVERD_PROCESS_CLI_STRING
} // namespace neverd::process_cli
#endif
