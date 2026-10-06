//===- DarwinSystem.h - Read-only system observation services ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_DARWIN_DARWINSYSTEM_H
#define NEVERD_EMULATION_OS_DARWIN_DARWINSYSTEM_H
#include "DarwinKernel.h"

namespace neverd::emulation::darwin_model {
llvm::Error validateSystemOptions(const DarwinSystemOptions &Options);
llvm::Expected<std::optional<ServiceResult>>
systemService(GuestMemory &Memory, uint64_t PageSize, ServiceKind Kind,
              const ProcessServiceEvent &Event,
              const std::optional<DarwinSystemOptions> &Options,
              ProcessResult &Result);
} // namespace neverd::emulation::darwin_model
#endif
