//===- ExecutionExit.h - One CPU run outcome -----------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_EXECUTIONEXIT_H
#define NEVERD_EMULATION_EXECUTIONEXIT_H

#include "neverd/emulation/BackendFault.h"
#include "neverd/emulation/ServiceRequest.h"

#include <string>

namespace neverd::emulation {
enum class ExecutionExitKind {
#define NEVERD_EXECUTION_EXIT(Name, Text) Name,
#include "neverd/emulation/ExecutionExit.def"
#undef NEVERD_EXECUTION_EXIT
};
const char *executionExitKindName(ExecutionExitKind Kind);

/// One completed CPU run, not a workload completion or OS exit status.
/// Guest/transport/device failures outrank a simultaneous stop or deadline;
/// the independent facts remain visible. EngineStop means the engine returned
/// without a known stop request, deadline or fault (for example a software
/// HLT); callers must not infer successful workload completion from it.
struct ExecutionExit {
  ExecutionExitKind Kind;
  std::optional<BackendFault> Fault;
  std::string Diagnostic;
  bool StopRequested = false;
  bool DeadlineReached = false;
  /// Present only for ServiceRequest. The CPU retains the request until its
  /// owner consumes it; returning this record does not release the CPU.
  std::optional<ServiceRequest> Service = std::nullopt;
};
} // namespace neverd::emulation
#endif
