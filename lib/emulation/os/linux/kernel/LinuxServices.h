//===- LinuxServices.h - Per-workload Linux service state -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_LINUX_LINUXSERVICES_H
#define NEVERD_EMULATION_OS_LINUX_LINUXSERVICES_H

#include "LinuxFiles.h"
#include "LinuxMemory.h"

namespace neverd::emulation::linux_model {

/// One workload's kernel state, shared by raw traps and libc wrappers.
/// Thread identity is supplied separately at each service boundary.
class LinuxServices {
public:
  LinuxServices(ExecutionBackend &CPU, const MemoryLayout &Layout,
                uint64_t InitialBreak, const ProcessOptions &Options,
                ProcessResult &Result)
      : CPU(CPU), Layout(Layout), Options(Options), Result(Result),
        Memory(*CPU.addressSpace(), Layout, InitialBreak, Options),
        Files(CPU, Layout, Options.LinuxFiles) {}

  llvm::Expected<std::optional<uint64_t>>
  handle(const ProcessServiceEvent &Event, ThreadContext *Thread = nullptr);
  llvm::Expected<std::optional<uint64_t>>
  handle(ServiceKind Kind, const ProcessServiceEvent &Event,
         ThreadContext *Thread = nullptr);

private:
  ExecutionBackend &CPU;
  const MemoryLayout &Layout;
  const ProcessOptions &Options;
  ProcessResult &Result;
  LinuxMemory Memory;
  LinuxFiles Files;
};

} // namespace neverd::emulation::linux_model

#endif
