//===- LinuxFileOptions.cpp - Memory-file catalogue validation ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxFiles.h"
#include "LinuxTime.h"

namespace neverd::emulation::linux_model {
llvm::Error validateFileOptions(const LinuxFileOptions &Options) {
  if (Options.DescriptorLimit < 3 ||
      Options.DescriptorLimit > FileDescriptorLimit ||
      Options.Files.size() > FileCountLimit)
    return failure(FileOptionsLimit);
  uint64_t Total = 0;
  for (const auto &[Path, Bytes] : Options.Files) {
    if (Path.size() >= FilePathLimit || !isCanonicalFilePath(Path))
      return failure(FileOptionPath);
    // A file cannot also be an ancestor directory of another file.
    for (size_t I = Path.find('/', 1); I != std::string::npos;
         I = Path.find('/', I + 1))
      if (Options.Files.contains(Path.substr(0, I)))
        return failure(FileOptionPath);
    const uint64_t Cost = Path.size() + 1;
    if (Cost > FileByteLimit - Total ||
        Bytes.size() > FileByteLimit - Total - Cost)
      return failure(FileOptionsLimit);
    Total += Cost + Bytes.size();
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
