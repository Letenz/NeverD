//===- DarwinLibSystem.h - Modeled libSystem call boundaries -----*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Exact imported function identities and private process call gates.
#ifndef NEVERD_EMULATION_OS_DARWIN_DARWINLIBSYSTEM_H
#define NEVERD_EMULATION_OS_DARWIN_DARWINLIBSYSTEM_H
#include "DarwinProcess.h"

#include "neverd/emulation/AddressSpace.h"

namespace neverd::emulation::darwin_model {
struct LibSystemBinding {
  llvm::StringLiteral Symbol;
  unsigned Arguments;
  std::optional<ServiceKind> Kernel;
};
std::optional<LibSystemGateKind> libSystemImport(llvm::StringRef Symbol);
const LibSystemBinding &libSystemBinding(LibSystemGateKind Kind);
llvm::Error prepareLibSystem(AddressSpace &Space, const ProcessImage &Image);
llvm::Expected<bool>
invokeLibSystem(ExecutionBackend &CPU, DarwinMemory &Memory, DarwinFiles &Files,
                DarwinEntropy &Entropy, const ProcessImage &Image,
                LibSystemGateKind Binding, const ServiceRequest &Request,
                const ProcessOptions &Options, ProcessResult &Result);
} // namespace neverd::emulation::darwin_model
#endif
