//===- LinuxTime.h - Shared explicit Linux clock inputs --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_LINUX_LINUXTIME_H
#define NEVERD_EMULATION_OS_LINUX_LINUXTIME_H
#include "LinuxKernel.h"

namespace neverd::emulation::linux_model {
#define NEVERD_LINUX_CLOCK(Name, ID) inline constexpr int32_t Name = ID;
#include "../LinuxValues.def"
#undef NEVERD_LINUX_CLOCK
llvm::Error validateTimeOptions(const LinuxTimeOptions &Options);
/// Missing input stops the workload without inventing a timestamp.
std::optional<LinuxTimespec>
clockValue(int32_t ID, const ProcessOptions &Options, ProcessResult &Result);
llvm::Expected<std::optional<uint64_t>>
timeService(ExecutionBackend &CPU, ServiceKind Kind,
            const ProcessServiceEvent &Event, const MemoryLayout &Layout,
            const ProcessOptions &Options, ProcessResult &Result);
} // namespace neverd::emulation::linux_model
#endif
