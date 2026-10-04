//===- LinuxProcess.h - Linux process model boundaries ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_LINUX_LINUXPROCESS_H
#define NEVERD_EMULATION_OS_LINUX_LINUXPROCESS_H

#include "../kernel/LinuxKernel.h"

#include "neverd/emulation/GuestMemory.h"
#include "neverd/emulation/IntegerABI.h"

namespace neverd {
struct BinaryImage;
namespace emulation::linux_model {
struct ProcessLayout {
  GuestArchitecture Architecture;
  IntegerABI Calls;
  MemoryLayout Memory;
  uint64_t ProgramHeaderAddress, LoadBias;
  bool ExecutableStack;
};
llvm::Expected<ProcessLayout> processLayout(const BinaryImage &Image);
llvm::Expected<uint64_t> prepareStack(GuestMemory &Memory,
                                      const BinaryImage &Image,
                                      const ProcessLayout &Layout,
                                      const ProcessOptions &Options,
                                      llvm::StringRef ExecutableName);
llvm::Expected<ProcessResult> runProcess(const std::filesystem::path &Path,
                                         const ProcessOptions &Options);
} // namespace emulation::linux_model
} // namespace neverd
#endif
