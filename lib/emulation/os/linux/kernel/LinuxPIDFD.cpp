//===- LinuxPIDFD.cpp - Workload-owned process descriptors ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxFiles.h"

namespace neverd::emulation::linux_model {
std::optional<uint64_t>
LinuxFiles::openProcessDescriptor(uint32_t PID, uint32_t Flags,
                                  uint32_t AllowedFlags,
                                  ProcessResult &Result) {
  // Kernel int arguments consume their low 32 bits. Flags are checked before
  // PID lookup, descriptor reservation or any possible output.
  if ((Flags & ~AllowedFlags) || PID == 0 || PID > INT32_MAX)
    return uint64_t(0) - InvalidArgument;
  // The running process is known to be a live group leader. No absent-process
  // or foreign-task identity is inferred from the lack of a host observation.
  if (PID != ProcessID)
    return unsupported(Result, PidFDTargetMissing);
  if (!Options)
    return unsupported(Result, FileInputsMissing);
  const uint32_t FD = nextDescriptor();
  if (FD == Options->DescriptorLimit)
    return uint64_t(0) - TooManyFiles;
  Descriptors.emplace(
      FD, ProcessDescriptor{PID, Flags | uint32_t(OpenCloseOnExec)});
  return FD;
}
} // namespace neverd::emulation::linux_model
