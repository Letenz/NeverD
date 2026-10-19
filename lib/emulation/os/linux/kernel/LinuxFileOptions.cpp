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
bool isDeclaredDirectory(const LinuxFileOptions &Options,
                         llvm::StringRef Path) {
  return Path == "/" ||
         (Path.size() < FilePathLimit && isCanonicalFilePath(Path) &&
          llvm::is_contained(Options.Directories, Path));
}
llvm::Error validateFileOptions(const LinuxFileOptions &Options) {
  if (Options.WorkingDirectory &&
      !isDeclaredDirectory(Options, *Options.WorkingDirectory))
    return failure("Linux working directory requires a declared directory");
  if (Options.IoctlTrace) {
    const auto &T = *Options.IoctlTrace;
    if (!T.EventLimit || T.EventLimit > 4096 || !T.ByteLimit ||
        T.ByteLimit > 1048576 || T.Requests.size() > 64)
      return failure("invalid ioctl trace bounds");
    for (const auto &[Request, Ranges] : T.Requests) {
      if (Ranges.size() > 16)
        return failure("too many ioctl trace ranges");
      for (const auto &R : Ranges)
        if (!R.Size || R.Size > 4096 || R.Offset > UINT64_MAX - R.Size ||
            (R.PointerOffset && *R.PointerOffset > UINT64_MAX - 8))
          return failure("invalid ioctl trace range");
    }
  }
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
  if (Options.DynamicProc) {
    const auto &P = *Options.DynamicProc;
    if (P.Name.empty() || P.Name.size() > 15 ||
        llvm::any_of(
            P.Name,
            [](unsigned char C) { return C < 32 || C == 127 || C == '\\'; }) ||
        P.ParentPID > INT32_MAX)
      return failure("invalid live proc identity");
    auto Reserved = [](llvm::StringRef Path, llvm::StringRef Prefix) {
      return Path == Prefix ||
             (Path.consume_front(Prefix) && Path.starts_with('/'));
    };
    for (const auto &Path : Paths)
      if (Path == "/proc" || Reserved(Path, "/proc/self") ||
          Reserved(Path, "/proc/thread-self") ||
          Reserved(Path, procProcessPath()))
        return failure("dynamic proc namespace conflicts with catalogue");
    if (P.Mappings.size() > FileCountLimit)
      return failure("proc backing identity budget exhausted");
    uint64_t PathBytes = 0;
    std::vector<std::pair<uint64_t, uint64_t>> Ranges;
    for (const auto &M : P.Mappings) {
      if (!M.Size || M.Size > UINT64_MAX - M.Address ||
          M.Size > UINT64_MAX - M.Offset || M.Path.size() >= FilePathLimit ||
          (!M.Path.empty() && !isCanonicalFilePath(M.Path)))
        return failure("invalid proc backing identity");
      PathBytes += M.Path.size() + 1;
      Ranges.emplace_back(M.Address, M.Address + M.Size);
    }
    if (PathBytes > FileByteLimit)
      return failure("proc backing identity budget exhausted");
    std::sort(Ranges.begin(), Ranges.end());
    for (size_t I = 1; I < Ranges.size(); ++I)
      if (Ranges[I].first < Ranges[I - 1].second)
        return failure("overlapping proc backing identities");
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
  if (Options.DirectoryCreation) {
    const auto &C = *Options.DirectoryCreation;
    if (C.Parents.empty() || C.Parents.size() > FileCountLimit || !C.Limit ||
        C.Limit > FileCountLimit || (C.Umask & ~0777u) ||
        C.Limit + Paths.size() > FileCountLimit ||
        uint64_t(C.Limit) * FilePathLimit > FileByteLimit - Total)
      return failure("invalid directory creation bounds or umask");
    std::set<uint64_t> Inodes;
    for (const auto &[Path, M] : Options.Metadata)
      Inodes.insert(M.Inode);
    for (const auto &[Path, M] : C.Parents) {
      if (!isDeclaredDirectory(Options, Path) ||
          ((Path == "/proc" || llvm::StringRef(Path).starts_with("/proc/")) &&
           (Options.DynamicProc || Options.DynamicProcFDs)))
        return failure("directory creation requires declared non-proc parents");
      if ((M.Mode & ~07777u) != 0040000 || (M.Mode & 04000) || !M.Inode ||
          !Inodes.insert(M.Inode).second || M.BlockSize != 4096 || M.Blocks ||
          !isNormalizedTimespec(M.AccessTime) ||
          !isNormalizedTimespec(M.ModificationTime) ||
          !isNormalizedTimespec(M.ChangeTime))
        return failure("invalid tmpfs directory metadata");
      const std::string Prefix = Path == "/" ? "/" : Path + '/';
      std::set<std::string> Entries, Directories;
      auto Add = [&](llvm::StringRef P, bool Directory) {
        if (!P.consume_front(Prefix) || P.empty())
          return;
        auto [Name, Tail] = P.split('/');
        Entries.insert(Name.str());
        if (Directory || !Tail.empty())
          Directories.insert(Name.str());
      };
      for (const auto &[P, B] : Options.Files)
        Add(P, false);
      for (const auto &[P, B] : Options.SymbolicLinks)
        Add(P, false);
      for (const auto &[P, D] : Options.Devices)
        Add(P, false);
      for (const auto &P : Options.Directories)
        Add(P, true);
      if (Options.DynamicProc || Options.DynamicProcFDs)
        Add("/proc", true);
      if (M.Size != (Entries.size() + 2) * 20 ||
          M.LinkCount != Directories.size() + 2)
        return failure(
            "directory metadata disagrees with the closed namespace");
    }
  }
  for (const auto &Path : Options.TrackWriteMetadata) {
    auto I = Options.Metadata.find(Path);
    if (!Options.WriteLimits.contains(Path) || I == Options.Metadata.end())
      return failure("tracked file metadata requires bytes, write reservation "
                     "and metadata");
    const auto &M = I->second;
    if (M.Size != Options.Files.at(Path).size() ||
        M.Blocks != ((M.Size + 4095) / 4096) * 8 || (M.Mode & 06000))
      return failure("tracked file metadata requires dense initial pages and "
                     "no set-ID bits");
  }
  return llvm::Error::success();
}

} // namespace neverd::emulation::linux_model
