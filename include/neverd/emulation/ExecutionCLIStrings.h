//===- ExecutionCLIStrings.h - Execution command-line vocabulary ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_EXECUTIONCLISTRINGS_H
#define NEVERD_EMULATION_EXECUTIONCLISTRINGS_H

namespace neverd::execution_cli {
#define NEVERD_EXECUTION_CLI_STRING(Name, Text)                                \
  inline constexpr char Name[] = Text;
#include "neverd/emulation/ExecutionCLIStrings.def"
#undef NEVERD_EXECUTION_CLI_STRING
} // namespace neverd::execution_cli

#endif
