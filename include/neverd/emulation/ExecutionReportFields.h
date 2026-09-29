//===- ExecutionReportFields.h - CPU query vocabulary --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_EXECUTIONREPORTFIELDS_H
#define NEVERD_EMULATION_EXECUTIONREPORTFIELDS_H

#include <cstdint>

namespace neverd::emulation::execution_report {
#define NEVERD_EXECUTION_CONFIG_FIELD(Name, Text)                              \
  inline constexpr char Name[] = Text;
#define NEVERD_EXECUTION_REPORT_FIELD(Name, Text)                              \
  inline constexpr char Name[] = Text;
#define NEVERD_EXECUTION_REPORT_TEXT(Name, Text)                               \
  inline constexpr char Name[] = Text;
#define NEVERD_EXECUTION_REPORT_LIMIT(Name, CName, Value)                      \
  inline constexpr uint64_t Name = Value;
#include "neverd/emulation/ExecutionReport.def"
#undef NEVERD_EXECUTION_REPORT_LIMIT
#undef NEVERD_EXECUTION_REPORT_TEXT
#undef NEVERD_EXECUTION_REPORT_FIELD
#undef NEVERD_EXECUTION_CONFIG_FIELD
} // namespace neverd::emulation::execution_report
#endif
