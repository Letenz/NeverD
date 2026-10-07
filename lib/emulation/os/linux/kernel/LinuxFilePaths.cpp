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
  if (Path == "/")
    return PathKind::Directory;
  if (Options->Files.contains(Path))
    return RequiresDirectory ? PathKind::NotDirectory : PathKind::File;
  // Catalogue prefixes denote directories; a known file used as a path
  // component is ENOTDIR.
  const std::string Prefix = Path + '/';
  auto Next = Options->Files.lower_bound(Prefix);
  if (Next != Options->Files.end() &&
      llvm::StringRef(Next->first).starts_with(Prefix))
    return PathKind::Directory;
  for (size_t I = Path.find('/', 1); I != std::string::npos;
       I = Path.find('/', I + 1))
    if (Options->Files.contains(Path.substr(0, I)))
      return PathKind::NotDirectory;
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
  case PathKind::Missing:
    return std::optional<uint64_t>(uint64_t(0) - NoEntry);
  case PathKind::NotDirectory:
    return std::optional<uint64_t>(uint64_t(0) - NotDirectory);
  case PathKind::File:
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
  case PathKind::Missing:
    return std::optional<uint64_t>(uint64_t(0) - NoEntry);
  case PathKind::File:
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

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::open(uint64_t Address, uint32_t Flags, ProcessResult &Result) {
  const uint32_t LargeFile = CPU.architecture() == GuestArchitecture::AArch64
                                 ? OpenLargeFileARM64
                                 : OpenLargeFileX64;
  if (Flags & ~(OpenCloseOnExec | LargeFile))
    return unsupported(Result, FileOpenFlags);
  auto Imported = readPath(Address);
  if (!Imported)
    return Imported.takeError();
  if (const auto *Error = std::get_if<uint32_t>(&*Imported))
    return std::optional<uint64_t>(uint64_t(0) - *Error);
  auto Path = parsePath(std::get<std::string>(*Imported));
  if (!Path)
    return unsupported(Result, FilePathForm);

  uint32_t FD = nextDescriptor();
  if (FD == Options->DescriptorLimit)
    return std::optional<uint64_t>(uint64_t(0) - TooManyFiles);
  switch (lookupPath(Path->Name, Path->RequiresDirectory)) {
  case PathKind::Missing:
    return std::optional<uint64_t>(uint64_t(0) - NoEntry);
  case PathKind::NotDirectory:
    return std::optional<uint64_t>(uint64_t(0) - NotDirectory);
  case PathKind::Directory:
    return unsupported(Result, FileDirectory);
  case PathKind::File:
    break;
  }
  auto File = Options->Files.find(Path->Name);
  auto Metadata = Options->Metadata.find(Path->Name);
  Descriptors.emplace(FD,
                      OpenFile{File->second, Metadata == Options->Metadata.end()
                                                 ? nullptr
                                                 : &Metadata->second});
  return std::optional<uint64_t>(FD);
}

} // namespace neverd::emulation::linux_model
