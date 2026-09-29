//===- ExecutionExitBuilder.h - Shared CPU exit precedence ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_EXECUTIONEXITBUILDER_H
#define NEVERD_EMULATION_EXECUTIONEXITBUILDER_H

#include "neverd/emulation/ExecutionExit.h"

#include "llvm/Support/Error.h"

namespace neverd::emulation {
struct ExecutionExitFacts {
  std::optional<BackendFault> Fault, Recoverable;
  bool DeviceFailed = false;
  /// Distinguish transport/observer failure from the synthetic terminal fault
  /// retained by legacy CPU APIs to prevent resumption.
  bool BackendFailed = false;
  bool InstructionRejected = false;
  bool StopRequested = false, DeadlineReached = false;
};

inline ExecutionExit makeExecutionExit(llvm::Error Error,
                                       const ExecutionExitFacts &Facts) {
  const bool Failed = bool(Error);
  ExecutionExit Result{ExecutionExitKind::EngineStop,
                       Facts.Fault ? Facts.Fault : Facts.Recoverable,
                       llvm::toString(std::move(Error)), Facts.StopRequested,
                       Facts.DeadlineReached};
  if (Facts.DeviceFailed)
    Result.Kind = ExecutionExitKind::DeviceFailure;
  else if (Facts.InstructionRejected)
    Result.Kind = ExecutionExitKind::UnsupportedOperation;
  else if (Facts.BackendFailed)
    Result.Kind = ExecutionExitKind::BackendFailure;
  else if (Facts.Fault)
    Result.Kind = Facts.Fault->Kind == BackendFaultKind::Interrupt
                      ? ExecutionExitKind::GuestTrap
                      : ExecutionExitKind::GuestFault;
  else if (Failed)
    Result.Kind = ExecutionExitKind::BackendFailure;
  else if (Facts.Recoverable)
    Result.Kind = ExecutionExitKind::RecoverableFault;
  else if (Facts.DeadlineReached)
    Result.Kind = ExecutionExitKind::Deadline;
  else if (Facts.StopRequested)
    Result.Kind = ExecutionExitKind::Stopped;
  return Result;
}
} // namespace neverd::emulation
#endif
