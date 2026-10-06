//===- UnpackCLIStrings.h - Unpack command-line vocabulary ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNPACK_UNPACKCLISTRINGS_H
#define NEVERD_UNPACK_UNPACKCLISTRINGS_H
namespace neverd::unpack_cli {
#define NEVERD_UNPACK_CLI_STRING(Name, Text)                                   \
  inline constexpr char Name[] = Text;
#define NEVERD_UNPACK_CLI_STATUS(Name, Value) inline constexpr int Name = Value;
#include "neverd/unpack/Unpack.def"
#undef NEVERD_UNPACK_CLI_STATUS
#undef NEVERD_UNPACK_CLI_STRING
} // namespace neverd::unpack_cli
#endif
