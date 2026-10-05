//===- DarwinUserMemory.h - Fixed Darwin user copies ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_DARWIN_DARWINUSERMEMORY_H
#define NEVERD_EMULATION_OS_DARWIN_DARWINUSERMEMORY_H

#include "DarwinKernel.h"

namespace neverd::emulation::darwin_model {
/// One fixed copy. An individually partial destination stops before copying;
/// completed earlier copies remain observable. No CPU fault is published.
llvm::Expected<std::optional<ServiceResult>>
copyUserMemory(GuestMemory &Memory, uint64_t Address,
               llvm::ArrayRef<uint8_t> Bytes, const char *PartialDiagnostic,
               ProcessResult &Result);
} // namespace neverd::emulation::darwin_model
#endif
