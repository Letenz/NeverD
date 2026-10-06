//===- UnpackStrings.h - Unpack report and diagnostic vocabulary *- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNPACK_UNPACKSTRINGS_H
#define NEVERD_UNPACK_UNPACKSTRINGS_H
#include <cstdint>

namespace neverd::unpack::strings {
#define NEVERD_UNPACK_TEXT(Name, Text) inline constexpr char Name[] = Text;
#define NEVERD_UNPACK_REPORT_FIELD(Name, Text)                                 \
  inline constexpr char Name##Field[] = Text;
#define NEVERD_UNPACK_REPORT_LIMIT(Name, CName, Value)                         \
  inline constexpr uint64_t Name = Value;
#define NEVERD_UNPACK_OUTCOME(Name, Text)                                      \
  inline constexpr char Name##Outcome[] = Text;
#include "neverd/unpack/Unpack.def"
#undef NEVERD_UNPACK_OUTCOME
#undef NEVERD_UNPACK_REPORT_LIMIT
#undef NEVERD_UNPACK_REPORT_FIELD
#undef NEVERD_UNPACK_TEXT
} // namespace neverd::unpack::strings
#endif
