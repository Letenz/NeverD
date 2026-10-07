//===- LinuxPriority.h - Workload-owned nice state --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_LINUX_LINUXPRIORITY_H
#define NEVERD_EMULATION_OS_LINUX_LINUXPRIORITY_H

#include "LinuxKernel.h"

namespace neverd::emulation::linux_model {
llvm::Error validatePriorityOptions(const LinuxPriorityOptions &Options);

class LinuxPriority {
public:
  explicit LinuxPriority(const std::optional<LinuxPriorityOptions> &Options)
      : Options(Options),
        Tasks(Options ? Options->Tasks : std::map<uint32_t, int32_t>{}) {}

  std::optional<uint64_t> handle(ServiceKind Kind,
                                 const ProcessServiceEvent &Event,
                                 uint64_t CurrentTask, ProcessResult &Result);

private:
  const std::optional<LinuxPriorityOptions> &Options;
  std::map<uint32_t, int32_t> Tasks;
  std::optional<uint64_t> unsupported(ProcessResult &Result,
                                      llvm::StringRef Text);
};
} // namespace neverd::emulation::linux_model
#endif
