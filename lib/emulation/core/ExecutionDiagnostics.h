//===- ExecutionDiagnostics.h - Execution errors -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_CORE_EXECUTIONDIAGNOSTICS_H
#define NEVERD_EMULATION_CORE_EXECUTIONDIAGNOSTICS_H
#include "MachineInterruptedError.h"
#include "MachineRunControl.h"

#include "neverd/emulation/ExecutionBackend.h"

namespace neverd::emulation {
namespace diagnostic {
#define NEVERD_EXECUTION_DIAGNOSTIC(Name, Text)                                \
  inline constexpr char Name[] = Text;
#include "ExecutionDiagnostics.def"
#undef NEVERD_EXECUTION_DIAGNOSTIC
inline llvm::Error error(const char *Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
inline llvm::Error interrupted(const char *Text, MachineRunControl Control) {
  const bool Stopped = Control.stopRequested();
  const bool Expired = std::chrono::steady_clock::now() >= Control.Deadline;
  if (!Stopped && !Expired)
    return error(Text);
  return llvm::make_error<MachineInterruptedError>(Text, Stopped, Expired);
}
inline llvm::Error unavailable(const char *Text,
                               BackendAvailability Availability =
                                   BackendAvailability::InitializationFailed) {
  return llvm::make_error<BackendUnavailableError>(Text, Availability);
}
} // namespace diagnostic
} // namespace neverd::emulation
#endif
