//===- RuntimeValues.h - Runtime units and diagnostics ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_RUNTIME_RUNTIMEVALUES_H
#define NEVERD_EMULATION_RUNTIME_RUNTIMEVALUES_H
#include <cstdint>
namespace neverd::emulation::runtime {
#define NEVERD_RUNTIME_VALUE(Name, Value)                                      \
  inline constexpr uint64_t Name = Value;
#define NEVERD_RUNTIME_DIAGNOSTIC(Name, Text)                                  \
  inline constexpr char Name[] = Text;
#include "RuntimeValues.def"
#undef NEVERD_RUNTIME_DIAGNOSTIC
#undef NEVERD_RUNTIME_VALUE
} // namespace neverd::emulation::runtime
#endif
