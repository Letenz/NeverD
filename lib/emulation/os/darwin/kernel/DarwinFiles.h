//===- DarwinFiles.h - Darwin file descriptions ----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_DARWIN_DARWINFILES_H
#define NEVERD_EMULATION_OS_DARWIN_DARWINFILES_H

#include "DarwinKernel.h"

#include <variant>

namespace neverd::emulation::darwin_model {
llvm::Error validateFileOptions(const DarwinFileOptions &Options);

class DarwinFiles {
public:
  DarwinFiles(GuestMemory &Memory,
              const std::optional<DarwinFileOptions> &Options);
  llvm::Expected<std::optional<ServiceResult>>
  handle(ServiceKind Kind, const ProcessServiceEvent &Event,
         ProcessResult &Result);
  /// Original capture sink, even when reached through a duplicated FD.
  std::optional<unsigned> outputSink(uint32_t FD) const;
  /// Immutable regular-file bytes, BSD errno, or an unsupported object kind.
  /// Looking up a mapping source never changes the open description's cursor.
  using MappingSource =
      std::variant<llvm::ArrayRef<uint8_t>, uint32_t, const char *>;
  MappingSource mappingSource(uint32_t FD) const;

private:
  enum class Kind { Input, Output, Error, File, Directory };
  struct Description {
    Kind Type;
    llvm::ArrayRef<uint8_t> Bytes;
    uint64_t Offset = 0;
    const DarwinFileMetadata *Metadata = nullptr;
    std::string Path;
  };
  struct Descriptor {
    std::shared_ptr<Description> Open;
    bool CloseOnExec = false;
  };
  using Pathname = std::variant<std::string, uint32_t>;
  using Lookup = std::variant<Description, uint32_t, const char *>;
  GuestMemory &Memory;
  const std::optional<DarwinFileOptions> &Options;
  std::map<uint32_t, Descriptor> Descriptors;
  std::optional<std::string> CurrentDirectory;

  uint32_t limit() const;
  uint32_t freeDescriptor(uint32_t Minimum = 0) const;
  llvm::Expected<Pathname> readPath(uint64_t Address);
  llvm::Expected<Lookup> resolvePath(uint64_t Address, uint32_t DirectoryFD);
  llvm::Expected<std::optional<ServiceResult>> open(uint64_t Address,
                                                    uint32_t Flags,
                                                    uint32_t DirectoryFD,
                                                    ProcessResult &Result);
  llvm::Expected<std::optional<ServiceResult>>
  status(const Description &File, uint64_t Address, ProcessResult &Result);
  llvm::Expected<std::optional<ServiceResult>>
  statusPath(uint64_t Path, uint64_t Address, uint32_t DirectoryFD,
             ProcessResult &Result);
  llvm::Expected<std::optional<ServiceResult>>
  copyout(uint64_t Address, llvm::ArrayRef<uint8_t> Bytes,
          const char *PartialDiagnostic, ProcessResult &Result);
  llvm::Expected<std::optional<ServiceResult>>
  read(Description &File, uint64_t Address, uint64_t Count, uint64_t Offset,
       bool Positioned, ProcessResult &Result);
  llvm::Expected<std::optional<ServiceResult>>
  directory(Description &File, uint64_t Address, uint64_t Count,
            uint64_t Position, ProcessResult &Result);
  ServiceResult seek(Description &File, uint64_t Offset, uint32_t Whence);
  ServiceResult duplicate(const Descriptor &Source, uint32_t Minimum,
                          bool CloseOnExec);
};
} // namespace neverd::emulation::darwin_model
#endif
