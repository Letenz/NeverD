//===- ExecutionLimits.h - Shared workload input ceilings ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared workload input ceilings.
///

#ifndef NEVERD_EMULATION_EXECUTIONLIMITS_H
#define NEVERD_EMULATION_EXECUTIONLIMITS_H
#include <cstdint>

namespace neverd::emulation::execution_limit {
#define NEVERD_EXECUTION_INPUT_LIMIT(Name, CName, Value)                       \
  inline constexpr uint64_t Name = Value;
#include "neverd/emulation/ExecutionInputLimits.def"
#undef NEVERD_EXECUTION_INPUT_LIMIT
} // namespace neverd::emulation::execution_limit
#endif
