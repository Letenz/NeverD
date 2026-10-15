//===- LinuxFiles.h - Workload-owned memory file descriptions ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_LINUX_LINUXFILES_H
#define NEVERD_EMULATION_OS_LINUX_LINUXFILES_H

#include "LinuxDeviceState.h"
#include "LinuxKernel.h"

#include <memory>
#include <set>
#include <variant>

namespace neverd::emulation::linux_model {
class LinuxMemory;
bool isCanonicalFilePath(llvm::StringRef Path);
llvm::Error validateFileOptions(const LinuxFileOptions &Options);
llvm::Error validateDeviceOptions(const LinuxDeviceOptions &Options,
                                  uint64_t &Bytes, uint64_t &Transactions);

class LinuxFiles {
public:
  LinuxFiles(ExecutionBackend &CPU, const MemoryLayout &Layout,
             const std::optional<LinuxFileOptions> &Options,
             LinuxMemory *Memory = nullptr)
      : CPU(CPU), Layout(Layout), Options(Options), Memory(Memory) {}

  llvm::Expected<std::optional<uint64_t>>
  handle(ServiceKind Kind, const ProcessServiceEvent &Event,
         ProcessResult &Result);
  llvm::Expected<std::optional<uint64_t>>
  extendedAttribute(ServiceKind Kind, const ProcessServiceEvent &Event,
                    const std::optional<LinuxKernelOptions> &Kernel,
                    ProcessResult &Result);
  struct DescriptorStatus {
    uint32_t AccessMode;
    std::optional<uint32_t> BlockSize;
    bool Directory = false;
    bool Append = false;
  };
  /// Open-description mode and unchanged filesystem block-size facts, or
  /// guest errno. This does not expose invalidated stat snapshots after writes.
  std::optional<std::variant<DescriptorStatus, uint32_t>>
  descriptorStatus(uint32_t FD, ProcessResult &Result) const;
  std::optional<uint64_t> outputError(uint32_t FD) const;
  uint32_t outputTarget(uint32_t FD) const;
  bool isDevice(uint32_t FD) const;
  bool isRegularFile(uint32_t FD) const;
  /// Missing descriptor returns EBADF; another descriptor kind returns null.
  /// The borrowed catalogue identity outlives every mapping in this process.
  std::variant<std::shared_ptr<LinuxDeviceState>, uint64_t>
  mappingDevice(uint32_t FD) const;
  std::optional<uint64_t>
  openProcessDescriptor(uint32_t PID, uint32_t Flags,
                        const LinuxKernelOptions &Kernel,
                        ProcessResult &Result);
  llvm::Expected<std::optional<uint64_t>>
  poll(const ProcessServiceEvent &Event,
       const std::optional<LinuxKernelOptions> &Kernel, ProcessResult &Result);

private:
  enum class Stream { Input, Output, Error };
  enum class PathKind {
    File,
    Device,
    Directory,
    Missing,
    NotDirectory,
    SymbolicLink,
    LinkTraversal
  };
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
    std::string Path;
    uint32_t Flags = 0;
  };
  struct MutableFile {
    std::vector<uint8_t> Bytes;
    bool Changed = false;
  };
  struct OpenDirectory {
    std::string Path;
    uint64_t Offset = 0;
  };
  struct ProcessDescriptor {
    uint32_t PID, Flags;
  };
  using OpenDevice = std::shared_ptr<LinuxDeviceState>;
  using Descriptor = std::variant<Stream, OpenFile, ProcessDescriptor,
                                  OpenDevice, OpenDirectory>;

  ExecutionBackend &CPU;
  const MemoryLayout &Layout;
  const std::optional<LinuxFileOptions> &Options;
  LinuxMemory *Memory;
  // Descriptor numbers refer to shared open descriptions. dup preserves the
  // cursor, device transaction position and context ownership; another open
  // creates a distinct description. VMAs also retain a device's lifetime.
  std::map<uint32_t, std::shared_ptr<Descriptor>> Descriptors{
      {0, std::make_shared<Descriptor>(Stream::Input)},
      {1, std::make_shared<Descriptor>(Stream::Output)},
      {2, std::make_shared<Descriptor>(Stream::Error)}};
  std::vector<std::weak_ptr<LinuxDeviceState>> DeviceDescriptions;
  std::map<std::string, MutableFile> MutableFiles;
  std::map<std::string, uint64_t> Inodes;
  std::set<uint64_t> UsedInodes;
  uint64_t NextInode = 1;

  llvm::ArrayRef<uint8_t> fileBytes(const OpenFile &File) const;
  std::optional<std::string> descriptorPath(uint32_t FD) const;
  static std::optional<uint32_t> procDescriptor(llvm::StringRef Path);
  llvm::Expected<uint64_t> inode(const std::string &Path);
  llvm::Expected<std::optional<uint64_t>>
  directoryEntries(uint32_t FD, uint64_t Address, uint32_t Size,
                   ProcessResult &Result);
  llvm::Expected<std::optional<uint64_t>>
  writeDescriptor(ServiceKind Kind, uint32_t FD, uint64_t Address,
                  uint64_t Size, ProcessResult &Result);

  static std::optional<uint64_t> unsupported(ProcessResult &Result,
                                             const char *Reason);
  uint32_t nextDescriptor() const;
  std::optional<uint16_t> pollReadiness(int32_t FD,
                                        ProcessResult &Result) const;
  llvm::Expected<Pathname> readPath(uint64_t Address, bool AllowEmpty = false);
  llvm::Expected<std::optional<uint64_t>> attributeNameError(uint64_t Address);
  static std::optional<ParsedPath> parsePath(llvm::StringRef Path);
  PathKind lookupPath(const std::string &Path,
                      bool RequiresDirectory = false) const;
  llvm::Expected<std::optional<uint64_t>>
  readLink(uint32_t Directory, uint64_t Path, uint64_t Address, uint32_t Size,
           ProcessResult &Result);
  llvm::Expected<std::optional<uint64_t>>
  access(uint64_t Address, uint32_t Mode, ProcessResult &Result);
  llvm::Expected<std::optional<uint64_t>> makeDirectory(uint64_t Address,
                                                        ProcessResult &Result);
  llvm::Expected<std::optional<uint64_t>>
  open(uint64_t Address, uint32_t Flags, ProcessResult &Result,
       uint32_t Directory = AtCurrentDirectory);
  llvm::Expected<uint64_t> read(OpenFile &File, uint64_t Address,
                                uint64_t Size);
  uint64_t seek(OpenFile &File, uint64_t Offset, uint32_t Whence);
  llvm::Expected<std::optional<uint64_t>> close(uint32_t FD,
                                                ProcessResult &Result);
  std::optional<uint64_t> duplicate(ServiceKind Kind, uint32_t Old,
                                    uint32_t New, uint32_t Flags,
                                    ProcessResult &Result);
  bool canRelease(const std::shared_ptr<Descriptor> &Description) const;
  llvm::Expected<std::optional<uint64_t>> contextIoctl(LinuxDeviceState &Device,
                                                       uint32_t Request,
                                                       uint64_t Argument,
                                                       ProcessResult &Result);
  llvm::Expected<std::optional<uint64_t>> deviceIoctl(uint32_t FD,
                                                      uint32_t Request,
                                                      uint64_t Argument,
                                                      ProcessResult &Result);
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
