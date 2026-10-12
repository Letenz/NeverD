//===- LinuxFileOptions.cpp - Memory-file catalogue validation ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxFiles.h"
#include "LinuxTime.h"

#include "llvm/ADT/STLExtras.h"

namespace neverd::emulation::linux_model {
llvm::Error validateFileOptions(const LinuxFileOptions &Options) {
  if (Options.DescriptorLimit < 3 ||
      Options.DescriptorLimit > FileDescriptorLimit ||
      Options.Files.size() > FileCountLimit ||
      Options.SymbolicLinks.size() > FileCountLimit - Options.Files.size())
    return failure(FileOptionsLimit);
  uint64_t Total = 0;
  for (const auto *Catalogue : {&Options.Files, &Options.SymbolicLinks}) {
    for (const auto &[Path, Bytes] : *Catalogue) {
      if (Path.size() >= FilePathLimit || !isCanonicalFilePath(Path) ||
          (Options.Files.contains(Path) &&
           Options.SymbolicLinks.contains(Path)))
        return failure(FileOptionPath);
      // Neither regular files nor links may also be catalogue directories.
      for (size_t I = Path.find('/', 1); I != std::string::npos;
           I = Path.find('/', I + 1)) {
        const auto Parent = Path.substr(0, I);
        if (Options.Files.contains(Parent) ||
            Options.SymbolicLinks.contains(Parent))
          return failure(FileOptionPath);
      }
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
