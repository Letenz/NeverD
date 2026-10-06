//===- DarwinFiles.h - Darwin file descriptions ----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_DARWIN_DARWINFILES_H
#define NEVERD_EMULATION_OS_DARWIN_DARWINFILES_H

#include "DarwinKernel.h"

#include "llvm/ADT/BitVector.h"

#include <variant>

namespace neverd::emulation::darwin_model {
llvm::Error validateFileOptions(const DarwinFileOptions &Options);

class DarwinFiles {
public:
  DarwinFiles(GuestMemory &Memory,
              const std::optional<DarwinFileOptions> &Options,
              uint64_t OutputLimit = process_defaults::Output);
  llvm::Expected<std::optional<ServiceResult>>
  handle(ServiceKind Kind, const ProcessServiceEvent &Event,
         ProcessResult &Result);
  struct Mapping {
    llvm::ArrayRef<uint8_t> Bytes;
    /// The VM owner retains this lease until every mapped range is unmapped.
    std::shared_ptr<const unsigned> Lease;
    bool Readable;
  };
  /// Current regular-file bytes, BSD errno, or an unsupported object kind.
  /// Looking up a mapping source never changes the open description's cursor.
  using MappingSource = std::variant<Mapping, uint32_t, const char *>;
  MappingSource mappingSource(uint32_t FD) const;

private:
  enum class Kind { Input, Output, Error, File, Directory, Missing };
  enum class MissingPath { Reject, Regular, Directory };
  enum class Terminal { Ordinary, Dot, DotDot };
  struct DirectoryIdentity {
    int32_t Device;
    uint32_t GID;
  };
  struct Contents {
    llvm::ArrayRef<uint8_t> Initial;
    std::optional<std::vector<uint8_t>> Modified;
    const DarwinFileMetadata *InitialMetadata = nullptr;
    std::optional<DarwinFileMetadata> CurrentMetadata;
    const DarwinFileMutationPolicy *Policy = nullptr;
    std::optional<llvm::BitVector> Allocated;
    bool Writable = false;
    /// Last linked name. All descriptions follow rename; unlink retains it.
    std::string Path;
    uint64_t PathCharge = 0;
    bool MetadataInvalidated = false;
    std::shared_ptr<const unsigned> Lease = std::make_shared<const unsigned>(0);
    llvm::ArrayRef<uint8_t> bytes() const {
      return Modified ? llvm::ArrayRef<uint8_t>(*Modified) : Initial;
    }
    const DarwinFileMetadata *metadata() const {
      return CurrentMetadata ? &*CurrentMetadata : InitialMetadata;
    }
  };
  struct Description {
    Kind Type;
    llvm::ArrayRef<uint8_t> Input;
    uint64_t Offset = 0;
    const DarwinFileMetadata *Metadata = nullptr;
    std::string Path;
    std::shared_ptr<Contents> File;
    uint32_t Flags = 0;
    Terminal FinalComponent = Terminal::Ordinary;
    llvm::ArrayRef<uint8_t> bytes() const {
      return File ? File->bytes() : Input;
    }
  };
  struct Descriptor {
    std::shared_ptr<Description> Open;
    bool CloseOnExec = false;
  };
  struct Buffer {
    uint64_t Address;
    uint64_t Size;
  };
  using VectorInput = std::variant<std::vector<Buffer>, uint32_t, const char *>;
  using Pathname = std::variant<std::string, uint32_t>;
  using Lookup = std::variant<Description, uint32_t, const char *>;
  GuestMemory &Memory;
  const std::optional<DarwinFileOptions> &Options;
  const uint64_t OutputLimit;
  std::map<uint32_t, Descriptor> Descriptors;
  std::map<std::string, std::shared_ptr<Contents>> Nodes;
  std::vector<std::shared_ptr<Contents>> Unlinked;
  std::map<std::string, std::optional<DirectoryIdentity>> CreatedDirectories;
  std::set<std::string> ChangedDirectories;
  bool NamespaceReady = false;
  uint64_t NextCreatedInode = 0;
  uint16_t CurrentUmask = 0;
  std::optional<uint64_t> StorageUsed;
  uint32_t FixedEntries = 0;
  std::optional<std::string> CurrentDirectory;

  uint32_t limit() const;
  uint32_t freeDescriptor(uint32_t Minimum = 0) const;
  void initializeNamespace();
  llvm::Error prepareMutation();
  void reclaimUnlinked();
  bool isDirectory(const std::string &Path) const;
  bool mutableDirectory(const std::string &Path) const;
  std::optional<DirectoryIdentity>
  directoryIdentity(const std::string &Path) const;
  llvm::Expected<Pathname> readPath(uint64_t Address);
  llvm::Expected<Lookup> resolvePath(uint64_t Address, uint32_t DirectoryFD,
                                     MissingPath Missing = MissingPath::Reject);
  llvm::Expected<std::optional<ServiceResult>>
  open(uint64_t Address, uint32_t Flags, uint32_t DirectoryFD, uint32_t Mode,
       ProcessResult &Result);
  llvm::Expected<std::optional<ServiceResult>> access(uint64_t Path,
                                                      uint32_t DirectoryFD,
                                                      uint32_t Mode,
                                                      ProcessResult &Result);
  llvm::Expected<std::optional<ServiceResult>>
  status(const Description &File, uint64_t Address, ProcessResult &Result);
  llvm::Expected<std::optional<ServiceResult>>
  statusPath(uint64_t Path, uint64_t Address, uint32_t DirectoryFD,
             ProcessResult &Result);
  static std::optional<uint32_t> rootRemovalError(const Description &File);
  llvm::Expected<std::optional<ServiceResult>>
  unlink(uint64_t Path, uint32_t DirectoryFD, ProcessResult &Result);
  llvm::Expected<std::optional<ServiceResult>>
  makeDirectory(uint64_t Path, uint32_t DirectoryFD, ProcessResult &Result);
  llvm::Expected<std::optional<ServiceResult>>
  removeDirectory(uint64_t Path, uint32_t DirectoryFD, ProcessResult &Result);
  llvm::Expected<std::optional<ServiceResult>>
  rename(uint64_t SourcePath, uint32_t SourceDirectory, uint64_t TargetPath,
         uint32_t TargetDirectory, ProcessResult &Result);
  void updateNamespaceMetadata(Contents &Node, bool Removed);
  llvm::Expected<std::optional<ServiceResult>>
  create(Description &File, uint32_t Mode, ProcessResult &Result);
  llvm::Expected<std::optional<ServiceResult>>
  copyout(uint64_t Address, llvm::ArrayRef<uint8_t> Bytes,
          const char *PartialDiagnostic, ProcessResult &Result);
  llvm::Expected<std::optional<ServiceResult>>
  read(Description &File, llvm::ArrayRef<Buffer> Buffers, uint64_t Count,
       uint64_t Offset, bool Positioned, ProcessResult &Result);
  llvm::Expected<std::optional<ServiceResult>>
  write(Description &File, llvm::ArrayRef<Buffer> Buffers, uint64_t Count,
        uint64_t Offset, bool Positioned, ProcessResult &Result);
  llvm::Expected<VectorInput> readVectors(uint64_t Address, uint32_t Count);
  llvm::Expected<std::optional<ServiceResult>>
  capture(Description &File, llvm::ArrayRef<Buffer> Buffers, uint64_t Count,
          bool Vectored, ProcessResult &Result);
  llvm::Expected<std::optional<ServiceResult>>
  resize(Description &File, uint64_t Size, ProcessResult &Result);
  llvm::Expected<std::optional<ServiceResult>>
  admitMutation(const Description &File, uint64_t Size, ProcessResult &Result);
  void publish(Description &File, std::vector<uint8_t> Bytes,
               std::optional<std::pair<uint64_t, uint64_t>> Written = {});
  llvm::Expected<std::optional<ServiceResult>>
  directory(Description &File, uint64_t Address, uint64_t Count,
            uint64_t Position, ProcessResult &Result);
  ServiceResult seek(Description &File, uint64_t Offset, uint32_t Whence);
  ServiceResult duplicate(const Descriptor &Source, uint32_t Minimum,
                          bool CloseOnExec);
};
} // namespace neverd::emulation::darwin_model
#endif
