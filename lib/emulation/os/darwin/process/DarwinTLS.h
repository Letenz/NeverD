//===- DarwinTLS.h - Main-thread Mach-O TLV lifetime ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Original TLS descriptor admission and one process-owned instantiation.
#ifndef NEVERD_EMULATION_DARWINTLS_H
#define NEVERD_EMULATION_DARWINTLS_H
#include "DarwinProcess.h"

#include "neverd/loader/MachO/MachOExecutionImage.h"
namespace neverd::emulation::darwin_model {
llvm::Error prepareTLSImage(ProcessImage &Out,
                            const MachOExecutionImage &Loaded,
                            const MachOProgramLinking &Facts, uint64_t Bias,
                            uint64_t &NextAddress, uint64_t Available,
                            uint64_t &Records,
                            llvm::function_ref<llvm::Error()> Check);
class DarwinTLS {
  const ProcessImage &Image;
  std::vector<bool> Instantiated;

public:
  explicit DarwinTLS(const ProcessImage &Image)
      : Image(Image), Instantiated(Image.TLS.size(), false) {}
  llvm::Expected<bool> invoke(ExecutionBackend &CPU, ExecutionBudget &Budget,
                              const ServiceRequest &Request,
                              ProcessResult &Result);
};
} // namespace neverd::emulation::darwin_model
#endif
