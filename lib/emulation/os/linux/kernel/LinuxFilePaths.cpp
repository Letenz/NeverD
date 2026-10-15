//===- LinuxFilePaths.cpp - Canonical guest pathname operations ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxFiles.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

namespace neverd::emulation::linux_model {
bool isCanonicalFilePath(llvm::StringRef Path) {
  if (!Path.consume_front("/") || Path.empty() || Path.contains('\0'))
    return false;
  llvm::SmallVector<llvm::StringRef> Components;
  Path.split(Components, '/');
  return llvm::all_of(Components, [](llvm::StringRef Part) {
    return !Part.empty() && Part != "." && Part != ".." &&
           Part.size() <= FileNameLimit;
  });
}

llvm::Expected<LinuxFiles::Pathname> LinuxFiles::readPath(uint64_t Address,
                                                          bool AllowEmpty) {
  // Import only through the first NUL, including at a readable page's last
  // byte. No host pathname conversion or filesystem lookup occurs.
  std::string Path;
  for (uint64_t I = 0; I < FilePathLimit; ++I) {
    if (Address >= Layout.UserLimit || I >= Layout.UserLimit - Address)
      return uint32_t(BadAddress);
    auto Access = CPU.canAccess(Address + I, 1, Read | UserAccessible);
    if (!Access)
      return Access.takeError();
    if (!*Access)
      return uint32_t(BadAddress);
    auto Byte = CPU.readInteger(Address + I, 1);
    if (!Byte)
      return Byte.takeError();
    if (!*Byte)
      break;
    Path.push_back(*Byte);
  }
  if (Path.size() == FilePathLimit)
    return uint32_t(NameTooLong);
  if (Path.empty() && !AllowEmpty)
    return uint32_t(NoEntry);
  return Path;
}

std::optional<LinuxFiles::ParsedPath>
LinuxFiles::parsePath(llvm::StringRef Path) {
  if (!Path.starts_with('/'))
    return std::nullopt;
  const bool RequiresDirectory = Path.ends_with('/');
  Path = Path.rtrim('/');
  if (Path.empty())
    return ParsedPath{"/", true};
  if (!isCanonicalFilePath(Path))
    return std::nullopt;
  return ParsedPath{Path.str(), RequiresDirectory};
}

LinuxFiles::PathKind LinuxFiles::lookupPath(const std::string &Path,
                                            bool RequiresDirectory) const {
  if (Path == "/" || llvm::is_contained(Options->Directories, Path))
    return PathKind::Directory;
  if (auto Kind = procKind(Path, RequiresDirectory))
    return *Kind;
  if (Options->DynamicProcFDs) {
    if (Path == "/proc" || Path == "/proc/self/fd")
      return PathKind::Directory;
    if (Path == "/proc/self")
      return RequiresDirectory ? PathKind::LinkTraversal
                               : PathKind::SymbolicLink;
    if (llvm::StringRef(Path).starts_with("/proc/self/fd/")) {
      auto FD = procDescriptor(Path);
      if (FD && Descriptors.contains(*FD))
        return RequiresDirectory ? PathKind::LinkTraversal
                                 : PathKind::SymbolicLink;
      return PathKind::Missing;
    }
  }
  if (Options->Files.contains(Path))
    return RequiresDirectory ? PathKind::NotDirectory : PathKind::File;
  if (Options->Devices.contains(Path))
    return RequiresDirectory ? PathKind::NotDirectory : PathKind::Device;
  if (Options->SymbolicLinks.contains(Path))
    return RequiresDirectory ? PathKind::LinkTraversal : PathKind::SymbolicLink;
  // Catalogue prefixes denote directories; a link component requires an
  // unmodeled traversal, while a regular file component is ENOTDIR.
  const std::string Prefix = Path + '/';
  for (const auto *Catalogue : {&Options->Files, &Options->SymbolicLinks}) {
    auto Next = Catalogue->lower_bound(Prefix);
    if (Next != Catalogue->end() &&
        llvm::StringRef(Next->first).starts_with(Prefix))
      return PathKind::Directory;
  }
  for (const auto &Directory : Options->Directories)
    if (llvm::StringRef(Directory).starts_with(Prefix))
      return PathKind::Directory;
  auto Device = Options->Devices.lower_bound(Prefix);
  if (Device != Options->Devices.end() &&
      llvm::StringRef(Device->first).starts_with(Prefix))
    return PathKind::Directory;
  for (size_t I = Path.find('/', 1); I != std::string::npos;
       I = Path.find('/', I + 1)) {
    const auto Parent = Path.substr(0, I);
    if (Options->Files.contains(Parent) || Options->Devices.contains(Parent))
      return PathKind::NotDirectory;
    if (Options->SymbolicLinks.contains(Parent))
      return PathKind::LinkTraversal;
  }
  return PathKind::Missing;
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::access(uint64_t Address, uint32_t Mode, ProcessResult &Result) {
  // Linux validates the int mode before importing the pathname. Credentials,
  // ACLs and mount policy are not implied by a file's metadata observation.
  if (Mode & ~uint32_t(FileAccessMask))
    return std::optional<uint64_t>(uint64_t(0) - InvalidArgument);
  auto Imported = readPath(Address);
  if (!Imported)
    return Imported.takeError();
  if (const auto *Error = std::get_if<uint32_t>(&*Imported))
    return std::optional<uint64_t>(uint64_t(0) - *Error);
  auto Path = parsePath(std::get<std::string>(*Imported));
  if (!Path)
    return unsupported(Result, FilePathForm);
  switch (lookupPath(Path->Name, Path->RequiresDirectory)) {
  case PathKind::SymbolicLink:
  case PathKind::LinkTraversal:
    return unsupported(Result, FileLinkTraversal);
  case PathKind::UnmodeledProc:
    return unsupported(Result, "proc entry has no modeled semantics");
  case PathKind::Missing:
    return std::optional<uint64_t>(uint64_t(0) - NoEntry);
  case PathKind::NotDirectory:
    return std::optional<uint64_t>(uint64_t(0) - NotDirectory);
  case PathKind::ProcFile:
  case PathKind::File:
  case PathKind::Device:
  case PathKind::Directory:
    if (Mode)
      return unsupported(Result, FileAccessMode);
    return std::optional<uint64_t>(0);
  }
  llvm_unreachable("unknown Linux catalogue entry kind");
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::makeDirectory(uint64_t Address, ProcessResult &Result) {
  auto Imported = readPath(Address);
  if (!Imported)
    return Imported.takeError();
  if (const auto *Error = std::get_if<uint32_t>(&*Imported))
    return std::optional<uint64_t>(uint64_t(0) - *Error);
  auto Parsed = parsePath(std::get<std::string>(*Imported));
  if (!Parsed)
    return unsupported(Result, FilePathForm);
  const auto &Path = Parsed->Name;

  // Linux resolves the parent and final component before applying mode,
  // umask or creation policy. Only these catalogue-derived failures are known.
  const size_t Slash = Path.rfind('/');
  const std::string Parent = Slash ? Path.substr(0, Slash) : "/";
  switch (lookupPath(Parent)) {
  case PathKind::SymbolicLink:
  case PathKind::LinkTraversal:
    return unsupported(Result, FileLinkTraversal);
  case PathKind::UnmodeledProc:
    return unsupported(Result, "proc entry has no modeled semantics");
  case PathKind::Missing:
    return std::optional<uint64_t>(uint64_t(0) - NoEntry);
  case PathKind::ProcFile:
  case PathKind::File:
  case PathKind::Device:
  case PathKind::NotDirectory:
    return std::optional<uint64_t>(uint64_t(0) - NotDirectory);
  case PathKind::Directory:
    break;
  }
  // Creation checks the final name itself: even a regular file followed by
  // slashes already exists. The directory requirement applies to its parent.
  if (lookupPath(Path) != PathKind::Missing)
    return std::optional<uint64_t>(uint64_t(0) - AlreadyExists);
  return unsupported(Result, FileDirectoryCreation);
}

llvm::Expected<std::optional<uint64_t>> LinuxFiles::open(uint64_t Address,
                                                         uint32_t Flags,
                                                         ProcessResult &Result,
                                                         uint32_t DirectoryFD) {
  const bool ARM64 = CPU.architecture() == GuestArchitecture::AArch64;
  const uint32_t LargeFile = ARM64 ? OpenLargeFileARM64 : OpenLargeFileX64;
  const uint32_t Directory = ARM64 ? OpenDirectoryARM64 : OpenDirectoryX64;
  const uint32_t Direct = ARM64 ? OpenDirectARM64 : OpenDirectX64;
  if ((Flags & Directory) && (Flags & OpenCreate))
    return unsupported(Result,
                       "O_CREAT with O_DIRECTORY is kernel-version dependent");
  const uint32_t AccessMask =
      3 | OpenCreate | OpenExclusive | OpenTruncate | OpenAppend;
  if ((Flags &
       ~(OpenCloseOnExec | LargeFile | Directory | Direct | AccessMask)) ||
      (Flags & 3) == 3)
    return unsupported(Result, FileOpenFlags);
  auto Imported = readPath(Address);
  if (!Imported)
    return Imported.takeError();
  if (const auto *Error = std::get_if<uint32_t>(&*Imported))
    return std::optional<uint64_t>(uint64_t(0) - *Error);
  std::string Name = std::get<std::string>(*Imported);
  if (!llvm::StringRef(Name).starts_with('/') &&
      DirectoryFD != AtCurrentDirectory) {
    auto I = Descriptors.find(DirectoryFD);
    if (I == Descriptors.end())
      return std::optional<uint64_t>(uint64_t(0) - BadDescriptor);
    const auto *D = std::get_if<OpenDirectory>(I->second.get());
    if (!D)
      return std::optional<uint64_t>(uint64_t(0) - NotDirectory);
    Name = D->Path + (D->Path == "/" ? "" : "/") + (Name == "." ? "" : Name);
  }
  auto Path = parsePath(Name);
  if (!Path)
    return unsupported(Result, FilePathForm);

  Path->Name = procPath(Path->Name);
  uint32_t FD = nextDescriptor();
  if (FD == Options->DescriptorLimit)
    return std::optional<uint64_t>(uint64_t(0) - TooManyFiles);
  const auto Kind =
      lookupPath(Path->Name, Path->RequiresDirectory || (Flags & Directory));
  if ((Flags & (OpenCreate | OpenExclusive)) == (OpenCreate | OpenExclusive) &&
      Kind != PathKind::Missing && Kind != PathKind::NotDirectory &&
      Kind != PathKind::LinkTraversal && Kind != PathKind::UnmodeledProc)
    return std::optional<uint64_t>(uint64_t(0) - AlreadyExists);
  switch (Kind) {
  case PathKind::SymbolicLink:
  case PathKind::LinkTraversal:
    return unsupported(Result, FileLinkTraversal);
  case PathKind::UnmodeledProc:
    return unsupported(Result, "proc entry has no modeled semantics");
  case PathKind::Missing:
    if (Flags & OpenCreate)
      return unsupported(Result,
                         "memory-file creation requires an explicit object");
    return std::optional<uint64_t>(uint64_t(0) - NoEntry);
  case PathKind::NotDirectory:
    return std::optional<uint64_t>(uint64_t(0) - NotDirectory);
  case PathKind::Directory:
    if ((Flags & 3) || (Flags & (OpenCreate | OpenTruncate)))
      return std::optional<uint64_t>(uint64_t(0) - IsDirectory);
    if (Flags & Direct)
      return unsupported(Result, FileDirectIO);
    Descriptors.emplace(
        FD, std::make_shared<Descriptor>(OpenDirectory{Path->Name, 0, Flags}));
    opened(FD, Flags);
    return std::optional<uint64_t>(FD);
  case PathKind::Device: {
    if (Flags & (OpenCreate | OpenExclusive | OpenTruncate | OpenAppend))
      return unsupported(Result, DeviceOpenMode);
    if (Flags & Direct)
      return unsupported(Result, FileDirectIO);
    const auto &Device = Options->Devices.at(Path->Name);
    if ((Flags & 3) != Device.AccessMode)
      return unsupported(Result, DeviceOpenMode);
    if (Device.OpenErrno)
      return std::optional<uint64_t>(uint64_t(0) - Device.OpenErrno);
    std::erase_if(DeviceDescriptions,
                  [](const auto &D) { return D.expired(); });
    auto Description =
        std::make_shared<LinuxDeviceState>(LinuxDeviceState{&Device});
    Description->OpenFlags = Flags;
    DeviceDescriptions.push_back(Description);
    Descriptors.emplace(FD,
                        std::make_shared<Descriptor>(std::move(Description)));
    opened(FD, Flags);
    return std::optional<uint64_t>(FD);
  }
  case PathKind::ProcFile:
    if ((Flags & 3) || (Flags & (OpenTruncate | OpenAppend | Direct)))
      return unsupported(Result, "proc write and direct IO are unmodeled");
    Descriptors.emplace(FD, std::make_shared<Descriptor>(
                                OpenProcFile{Path->Name, 0, 0, Flags}));
    opened(FD, Flags);
    return std::optional<uint64_t>(FD);
  case PathKind::File:
    break;
  }
  // The kernel resolves the pathname before checking an opened file's direct
  // I/O support. A missing path or non-directory component is already known;
  // this catalogue does not infer a backing filesystem's direct-I/O policy.
  if (Flags & Direct)
    return unsupported(Result, FileDirectIO);
  if ((Flags & 3) || (Flags & OpenTruncate)) {
    if (!Options->WriteLimits.contains(Path->Name))
      return unsupported(Result,
                         "memory-file write requires an explicit reservation");
    auto &Object = mutableFile(Path->Name);
    if (Flags & OpenTruncate) {
      if (!touchWrite(Object, Result))
        return std::optional<uint64_t>();
      Object.Bytes.clear();
      Object.Changed = true;
      Object.AllocatedPages.clear();
      if (Object.Metadata) {
        Object.Metadata->Size = 0;
        Object.Metadata->Blocks = 0;
      }
    }
  }
  auto File = Options->Files.find(Path->Name);
  auto Metadata = Options->Metadata.find(Path->Name);
  Descriptors.emplace(
      FD, std::make_shared<Descriptor>(OpenFile{
              File->second,
              Metadata == Options->Metadata.end() ? nullptr : &Metadata->second,
              0, Path->Name, Flags}));
  opened(FD, Flags);
  return std::optional<uint64_t>(FD);
}

} // namespace neverd::emulation::linux_model
