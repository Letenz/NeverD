//===- LinuxSignals.h - Workload-owned signal dispositions ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_LINUX_LINUXSIGNALS_H
#define NEVERD_EMULATION_OS_LINUX_LINUXSIGNALS_H

#include "LinuxKernel.h"

namespace neverd::emulation::linux_model {
llvm::Error validateSignalOptions(const LinuxSignalOptions &Options);

struct SignalActionResult {
  uint64_t Status = 0;
  LinuxSignalAction Previous;
};

/// Raw syscalls and libc adapters share the same process-wide action table.
/// There are no pending signals, host handlers, signal frames or delivery.
class LinuxSignals {
public:
  explicit LinuxSignals(const std::optional<LinuxSignalOptions> &Options)
      : Enabled(Options.has_value()),
        State(Options.value_or(LinuxSignalOptions{})) {}

  std::optional<SignalActionResult> action(int32_t Signal,
                                           const LinuxSignalAction *NewAction,
                                           bool ReadOld, ProcessResult &Result);
  llvm::Expected<std::optional<uint64_t>>
  handle(ExecutionBackend &CPU, const MemoryLayout &Layout,
         const ProcessServiceEvent &Event, ProcessResult &Result);

private:
  bool Enabled;
  LinuxSignalOptions State;
};
} // namespace neverd::emulation::linux_model
#endif
