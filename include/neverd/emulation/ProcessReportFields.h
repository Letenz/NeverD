//===- ProcessReportFields.h - Process wire vocabulary ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_PROCESSREPORTFIELDS_H
#define NEVERD_EMULATION_PROCESSREPORTFIELDS_H
#include <cstdint>

namespace neverd::emulation::process_report {
#define NEVERD_PROCESS_OPTION_NUMBER(Name, Text, Member)                       \
  inline constexpr char Name[] = Text;
#define NEVERD_PROCESS_OPTION_STRINGS(Name, Text, Member)                      \
  inline constexpr char Name[] = Text;
#define NEVERD_PROCESS_REPORT_FIELD(Name, Text)                                \
  inline constexpr char Name[] = Text;
#define NEVERD_PROCESS_REPORT_TEXT(Name, Text)                                 \
  inline constexpr char Name[] = Text;
#define NEVERD_PROCESS_REPORT_LIMIT(Name, CName, Value)                        \
  inline constexpr uint64_t Name = Value;
#include "neverd/emulation/ProcessReport.def"
#undef NEVERD_PROCESS_REPORT_LIMIT
#undef NEVERD_PROCESS_REPORT_TEXT
#undef NEVERD_PROCESS_REPORT_FIELD
#undef NEVERD_PROCESS_OPTION_STRINGS
#undef NEVERD_PROCESS_OPTION_NUMBER
} // namespace neverd::emulation::process_report
namespace neverd::emulation::process_outcome {
#define NEVERD_PROCESS_STOP(Name, Text) inline constexpr char Name[] = Text;
#include "neverd/emulation/ProcessProfile.def"
#undef NEVERD_PROCESS_STOP
} // namespace neverd::emulation::process_outcome
#endif
