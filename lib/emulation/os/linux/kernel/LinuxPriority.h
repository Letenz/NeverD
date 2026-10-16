//===- LinuxPriority.h - Workload-owned nice state --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_LINUX_LINUXPRIORITY_H
#define NEVERD_EMULATION_OS_LINUX_LINUXPRIORITY_H

#include "LinuxKernel.h"

#include <set>

namespace neverd::emulation::linux_model {
llvm::Error validatePriorityOptions(const LinuxPriorityOptions &Options);

class LinuxPriority {
public:
  explicit LinuxPriority(const std::optional<LinuxPriorityOptions> &Options)
      : Options(Options) {
    if (Options)
      for (const auto &[ID, Nice] : Options->Tasks)
        Tasks.emplace(ID, Nice);
  }

  /// Copy the parent's current nice value before publishing a new thread.
  /// Unknown parent state stays unknown. A failed publication must cancel it.
  llvm::Error inherit(uint64_t Parent, uint64_t Child);
  void cancel(uint64_t Child) { Tasks.erase(Child); }
  void exit(uint64_t Child);

  std::optional<uint64_t> handle(ServiceKind Kind,
                                 const ProcessServiceEvent &Event,
                                 uint64_t CurrentTask, ProcessResult &Result);

private:
  const std::optional<LinuxPriorityOptions> &Options;
  std::map<uint64_t, std::optional<int32_t>> Tasks;
  std::set<uint64_t> Exited;
  std::optional<uint64_t> unsupported(ProcessResult &Result,
                                      llvm::StringRef Text);
};
} // namespace neverd::emulation::linux_model
#endif
