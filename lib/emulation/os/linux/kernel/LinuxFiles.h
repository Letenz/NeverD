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
bool isCanonicalFilePath(llvm::StringRef Path);
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
  enum class PathKind { File, Directory, Missing, NotDirectory };
  // A pathname import can fail with guest errno independently of backend I/O.
  using Pathname = std::variant<std::string, uint32_t>;
  struct ParsedPath {
    std::string Name;
    bool RequiresDirectory;
  };
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

  static std::optional<uint64_t> unsupported(ProcessResult &Result,
                                             const char *Reason);
  llvm::Expected<Pathname> readPath(uint64_t Address, bool AllowEmpty = false);
  static std::optional<ParsedPath> parsePath(llvm::StringRef Path);
  PathKind lookupPath(const std::string &Path,
                      bool RequiresDirectory = false) const;
  llvm::Expected<std::optional<uint64_t>>
  access(uint64_t Address, uint32_t Mode, ProcessResult &Result);
  llvm::Expected<std::optional<uint64_t>> makeDirectory(uint64_t Address,
                                                        ProcessResult &Result);
  llvm::Expected<std::optional<uint64_t>> open(uint64_t Address, uint32_t Flags,
                                               ProcessResult &Result);
  llvm::Expected<uint64_t> read(OpenFile &File, uint64_t Address,
                                uint64_t Size);
  uint64_t seek(OpenFile &File, uint64_t Offset, uint32_t Whence);
  llvm::Expected<std::optional<uint64_t>> close(uint32_t FD);
  llvm::Expected<std::optional<uint64_t>> readDescriptor(uint32_t FD,
                                                         uint64_t Address,
                                                         uint64_t Size,
                                                         ProcessResult &Result);
  llvm::Expected<std::optional<uint64_t>> seekDescriptor(uint32_t FD,
                                                         uint64_t Offset,
                                                         uint32_t Whence,
                                                         ProcessResult &Result);
  llvm::Expected<std::optional<uint64_t>>
  status(const LinuxFileMetadata &Metadata, uint64_t Address,
         ProcessResult &Result);
  llvm::Expected<std::optional<uint64_t>>
  statusDescriptor(uint32_t FD, uint64_t Address, ProcessResult &Result);
  llvm::Expected<std::optional<uint64_t>>
  statusAt(uint32_t Directory, uint64_t Path, uint64_t Address, uint32_t Flags,
           ProcessResult &Result);
  llvm::Expected<std::optional<uint64_t>>
  fileSystemStatus(uint64_t Path, ProcessResult &Result);
  std::optional<uint64_t> fileSystemStatusDescriptor(uint32_t FD,
                                                     ProcessResult &Result);
};
} // namespace neverd::emulation::linux_model
#endif
