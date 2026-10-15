//===- LinuxFileOptions.cpp - Memory-file catalogue validation ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxFiles.h"
#include "LinuxTime.h"

#include "llvm/ADT/STLExtras.h"

#include <set>

namespace neverd::emulation::linux_model {
llvm::Error validateFileOptions(const LinuxFileOptions &Options) {
  if (Options.DescriptorLimit < 3 ||
      Options.DescriptorLimit > FileDescriptorLimit ||
      Options.Directories.size() + Options.Files.size() +
              Options.SymbolicLinks.size() + Options.Devices.size() >
          FileCountLimit ||
      Options.Files.size() > FileCountLimit ||
      Options.SymbolicLinks.size() > FileCountLimit - Options.Files.size() ||
      Options.Devices.size() >
          FileCountLimit - Options.Files.size() - Options.SymbolicLinks.size())
    return failure(FileOptionsLimit);
  std::set<std::string> Paths;
  auto AddPaths = [&](const auto &Catalogue) -> llvm::Error {
    for (const auto &[Path, Value] : Catalogue)
      if (Path.size() >= FilePathLimit || !isCanonicalFilePath(Path) ||
          !Paths.insert(Path).second)
        return failure(FileOptionPath);
    return llvm::Error::success();
  };
  if (auto E = AddPaths(Options.Files))
    return E;
  if (auto E = AddPaths(Options.SymbolicLinks))
    return E;
  if (auto E = AddPaths(Options.Devices))
    return E;
  for (const auto &Path : Paths)
    for (size_t I = Path.find('/', 1); I != std::string::npos;
         I = Path.find('/', I + 1))
      if (Paths.contains(Path.substr(0, I)))
        return failure(FileOptionPath);
  for (const auto &Path : Options.Directories) {
    if (Path.size() >= FilePathLimit || !isCanonicalFilePath(Path) ||
        !Paths.insert(Path).second)
      return failure(FileOptionPath);
    for (size_t I = Path.find('/', 1); I != std::string::npos;
         I = Path.find('/', I + 1)) {
      const auto Parent = Path.substr(0, I);
      if (Options.Files.contains(Parent) || Options.Devices.contains(Parent) ||
          Options.SymbolicLinks.contains(Parent))
        return failure(FileOptionPath);
    }
  }
  if (Options.DynamicProcFDs)
    for (const auto &Path : Paths)
      if (Path == "/proc" || Path == "/proc/self" || Path == "/proc/self/fd" ||
          llvm::StringRef(Path).starts_with("/proc/self/fd/"))
        return failure("dynamic proc FD namespace conflicts with catalogue");
  uint64_t Total = 0;
  for (const auto &Path : Options.Directories)
    Total += Path.size() + 1;
  for (const auto &[Path, Limit] : Options.WriteLimits) {
    auto F = Options.Files.find(Path);
    if (F == Options.Files.end() || Limit < F->second.size() ||
        Limit > FileByteLimit - Total)
      return failure("invalid memory-file write reservation");
    Total += Limit - F->second.size();
  }
  for (const auto *Catalogue : {&Options.Files, &Options.SymbolicLinks}) {
    for (const auto &[Path, Bytes] : *Catalogue) {
      const uint64_t Cost = Path.size() + 1;
      if (Cost > FileByteLimit - Total ||
          Bytes.size() > FileByteLimit - Total - Cost)
        return failure(FileOptionsLimit);
      Total += Cost + Bytes.size();
      if (Catalogue == &Options.SymbolicLinks &&
          (Bytes.empty() || llvm::is_contained(Bytes, uint8_t(0))))
        return failure(FileLinkTarget);
    }
  }
  uint64_t Transactions = 0;
  for (const auto &[Path, Device] : Options.Devices) {
    if (Path.size() + 1 > FileByteLimit - Total)
      return failure(FileOptionsLimit);
    Total += Path.size() + 1;
    if (auto E = validateDeviceOptions(Device, Total, Transactions))
      return E;
  }
  for (const auto &[Path, Metadata] : Options.Metadata) {
    if (!Options.Files.contains(Path))
      return failure(FileMetadataPath);
    if ((Metadata.Mode & ~FilePermissionMask) != FileRegularMode ||
        Metadata.Size > MaxSignedIOSize || Metadata.Blocks > MaxSignedIOSize ||
        Metadata.BlockSize > INT32_MAX ||
        !isNormalizedTimespec(Metadata.AccessTime) ||
        !isNormalizedTimespec(Metadata.ModificationTime) ||
        !isNormalizedTimespec(Metadata.ChangeTime))
      return failure(FileMetadataOption);
  }
  return llvm::Error::success();
}

} // namespace neverd::emulation::linux_model
