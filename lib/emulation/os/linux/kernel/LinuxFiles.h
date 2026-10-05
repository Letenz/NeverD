//===- LinuxFiles.h - Workload-owned memory file descriptions ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_LINUX_LINUXFILES_H
#define NEVERD_EMULATION_OS_LINUX_LINUXFILES_H

#include "LinuxKernel.h"

#include <variant>

namespace neverd::emulation::linux_model {
llvm::Error validateFileOptions(const LinuxFileOptions &Options);

class LinuxFiles {
public:
  LinuxFiles(ExecutionBackend &CPU, const MemoryLayout &Layout,
             const std::optional<LinuxFileOptions> &Options)
      : CPU(CPU), Layout(Layout), Options(Options) {}

  llvm::Expected<std::optional<uint64_t>>
  handle(ServiceKind Kind, const ProcessServiceEvent &Event,
         ProcessResult &Result);
  bool isOutput(uint32_t FD) const;

private:
  enum class Stream { Input, Output, Error };
  struct OpenFile {
    llvm::ArrayRef<uint8_t> Bytes;
    const LinuxFileMetadata *Metadata = nullptr;
    uint64_t Offset = 0;
  };
  using Descriptor = std::variant<Stream, OpenFile>;

  ExecutionBackend &CPU;
  const MemoryLayout &Layout;
  const std::optional<LinuxFileOptions> &Options;
  std::map<uint32_t, Descriptor> Descriptors{
      {0, Stream::Input}, {1, Stream::Output}, {2, Stream::Error}};

  llvm::Expected<std::optional<uint64_t>> open(uint64_t Address, uint32_t Flags,
                                               ProcessResult &Result);
  llvm::Expected<uint64_t> read(OpenFile &File, uint64_t Address,
                                uint64_t Size);
  uint64_t seek(OpenFile &File, uint64_t Offset, uint32_t Whence);
  llvm::Expected<std::optional<uint64_t>>
  status(const OpenFile &File, uint64_t Address, ProcessResult &Result);
};
} // namespace neverd::emulation::linux_model
#endif
