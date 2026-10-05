//===- DarwinTime.h - Darwin time validation and services -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_DARWIN_DARWINTIME_H
#define NEVERD_EMULATION_OS_DARWIN_DARWINTIME_H

#include "DarwinKernel.h"

namespace neverd::emulation::darwin_model {
llvm::Error validateTimeOptions(const DarwinTimeOptions &Options);
llvm::Expected<std::optional<ServiceResult>>
timeService(GuestMemory &Memory, const ProcessServiceEvent &Event,
            const std::optional<DarwinTimeOptions> &Options,
            ProcessResult &Result);
} // namespace neverd::emulation::darwin_model
#endif
