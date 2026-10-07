//===- LinuxPIDFD.cpp - Workload-owned process descriptors ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxFiles.h"
#include "LinuxKernelAvailability.h"

#include <cassert>

namespace neverd::emulation::linux_model {
std::optional<uint64_t>
LinuxFiles::openProcessDescriptor(uint32_t PID, uint32_t Flags,
                                  const LinuxKernelOptions &Kernel,
                                  ProcessResult &Result) {
  assert(Kernel.GKI && "released kernel validated before execution");
  const auto AllowedFlags = gkiPidFDFlags(*Kernel.GKI);
  assert(AllowedFlags && "released kernel validated before execution");
  // Kernel int arguments consume their low 32 bits. Flags are checked before
  // PID lookup, descriptor reservation or any possible output.
  if ((Flags & ~*AllowedFlags) || PID == 0 || PID > INT32_MAX)
    return uint64_t(0) - InvalidArgument;
  // The running process is always a live group leader. Only an explicit closed
  // guest catalogue establishes the presence or absence of another task.
  if (PID != ProcessID) {
    if (!Kernel.Tasks)
      return unsupported(Result, PidFDTargetMissing);
    const auto Task = Kernel.Tasks->find(PID);
    if (Task == Kernel.Tasks->end())
      return uint64_t(0) - NoSuchProcess;
    if (!Task->second.GroupLeader && !(Flags & PidFDThread)) {
      const auto Error = gkiPidFDNonLeaderError(*Kernel.GKI);
      assert(Error && "released kernel validated before execution");
      return uint64_t(0) - *Error;
    }
  }
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
