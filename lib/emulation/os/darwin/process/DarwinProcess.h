//===- DarwinProcess.h - Shared Darwin process contracts --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_DARWIN_DARWINPROCESS_H
#define NEVERD_EMULATION_OS_DARWIN_DARWINPROCESS_H

#include "../kernel/DarwinKernel.h"

#include "neverd/emulation/ImageMapping.h"
#include "neverd/emulation/IntegerABI.h"

namespace neverd::emulation::darwin_model {
struct ProfileSpec {
  ProcessProfile Profile;
  uint32_t Platform;
  bool AllowX64;
};
ProfileSpec macOSProfile();
ProfileSpec iOSProfile(bool Simulator);
struct ProcessImage {
  GuestArchitecture Architecture;
  MemoryLayout Memory;
  bool MainEntry;
  ImageMappingPlan Plan;
};
struct InitialStack {
  uint64_t SP, Argc, Argv, Envp, Apple;
};
llvm::Expected<ProcessImage> loadImage(const std::filesystem::path &Path,
                                       ProfileSpec Profile,
                                       const ProcessOptions &Options);
llvm::Expected<InitialStack> prepareStack(GuestMemory &Memory,
                                          const ProcessOptions &Options,
                                          llvm::StringRef ExecutableName);
llvm::Expected<ProcessResult> runProcess(const std::filesystem::path &Path,
                                         ProfileSpec Profile,
                                         const ProcessOptions &Options);
} // namespace neverd::emulation::darwin_model
#endif
