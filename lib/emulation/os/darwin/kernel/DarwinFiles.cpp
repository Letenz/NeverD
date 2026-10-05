//===- DarwinFiles.cpp - Workload-owned Darwin file services --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Original model of the BSD ABI described in the pinned XNU sources linked
// from docs/darwin-emulation.md. No host descriptors or filesystem calls.
#include "DarwinFiles.h"

#include "DarwinDirectory.h"
#include "DarwinUserMemory.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation::darwin_model {
using namespace value;
namespace {
namespace limits = darwin_file_limits;
bool canonicalPath(llvm::StringRef Path, bool AllowRoot = false) {
  if (AllowRoot && Path == "/")
    return true;
  if (!Path.consume_front("/") || Path.empty() || Path.contains('\0'))
    return false;
  llvm::SmallVector<llvm::StringRef> Parts;
  Path.split(Parts, '/');
  return llvm::all_of(Parts, [](llvm::StringRef Part) {
    return !Part.empty() && Part != "." && Part != "..";
  });
}
enum class PathKind { Missing, File, Directory };
std::string parentPath(const std::string &Path) {
  return Path.substr(0, std::max<size_t>(1, Path.rfind('/')));
}
PathKind pathKind(const DarwinFileOptions &Options, const std::string &Path) {
  if (Options.Files.contains(Path))
    return PathKind::File;
  if (Path == "/" || Options.Directories.contains(Path))
    return PathKind::Directory;
  const auto Prefix = Path + '/';
  auto File = Options.Files.lower_bound(Prefix);
  auto Directory = Options.Directories.lower_bound(Prefix);
  if ((File != Options.Files.end() &&
       llvm::StringRef(File->first).starts_with(Prefix)) ||
      (Directory != Options.Directories.end() &&
       llvm::StringRef(*Directory).starts_with(Prefix)))
    return PathKind::Directory;
  return PathKind::Missing;
}
std::optional<ServiceResult> returned(uint64_t Value, bool Error = false) {
  return ServiceResult{Value, Error};
}
std::optional<ServiceResult> unsupported(ProcessResult &Result,
                                         const char *Reason) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = Reason;
  return std::nullopt;
}
struct FileFootprint {
  uint64_t Bytes;
  uint32_t Entries;
};
bool validTime(const DarwinFileTime &Time) {
  return Time.Nanoseconds >= 0 && Time.Nanoseconds < 1000000000;
}
bool validMutationPolicy(const DarwinFileMutationPolicy &Policy) {
  const auto Unit = Policy.AllocationUnit;
  return Unit >= 512 && Unit <= limits::Bytes && !(Unit & (Unit - 1)) &&
         validTime(Policy.Time);
}
llvm::Expected<FileFootprint>
fileOptionsFootprint(const DarwinFileOptions &Options) {
  if (Options.InitialUmask && *Options.InitialUmask > 07777)
    return failure(diagnostic::FileUmaskOption);
  uint64_t Entries = Options.Files.size() + Options.Directories.size();
  if (Options.DescriptorLimit < 3 ||
      Options.DescriptorLimit > limits::Descriptors || Entries > limits::Files)
    return failure(diagnostic::FileOptionsLimit);
  uint64_t Total = Options.StandardInput ? Options.StandardInput->size() : 0;
  if (Total > limits::Bytes)
    return failure(diagnostic::FileOptionsLimit);
  auto PathInput = [&](const std::string &Path, bool Directory,
                       uint64_t Bytes = 0) -> llvm::Error {
    if (Path.size() >= limits::Path || !canonicalPath(Path, Directory))
      return failure(diagnostic::FileOptionPath);
    llvm::SmallVector<llvm::StringRef> Parts;
    llvm::StringRef(Path).split(Parts, '/');
    if (llvm::any_of(Parts,
                     [](llvm::StringRef P) { return P.size() > limits::Name; }))
      return failure(diagnostic::FileOptionPath);
    if (Directory && Options.Files.contains(Path))
      return failure(diagnostic::FileOptionPath);
    for (size_t I = Path.find('/', 1); I != std::string::npos;
         I = Path.find('/', I + 1))
      if (Options.Files.contains(Path.substr(0, I)))
        return failure(diagnostic::FileOptionPath);
    const uint64_t Cost = Path.size() + 1;
    if (Cost > limits::Bytes - Total || Bytes > limits::Bytes - Total - Cost)
      return failure(diagnostic::FileOptionsLimit);
    Total += Cost + Bytes;
    return llvm::Error::success();
  };
  for (const auto &[Path, Bytes] : Options.Files)
    if (auto E = PathInput(Path, false, Bytes.size()))
      return E;
  for (const auto &Path : Options.Directories)
    if (auto E = PathInput(Path, true))
      return E;
  for (const auto &[Path, M] : Options.Metadata) {
    const auto Kind = pathKind(Options, Path);
    if (Kind == PathKind::Missing)
      return failure(diagnostic::FileMetadataPath);
    if (Kind == PathKind::Directory && !Options.Directories.contains(Path)) {
      if (++Entries > limits::Files)
        return failure(diagnostic::FileOptionsLimit);
      if (auto E = PathInput(Path, true))
        return E;
    }
    const auto Mode =
        Kind == PathKind::File ? FileRegularMode : FileDirectoryMode;
    if ((M.Mode & ~FilePermissionMask) != Mode || M.Size > INT64_MAX ||
        (Kind == PathKind::File && M.Size != Options.Files.at(Path).size()) ||
        M.Blocks > INT64_MAX || M.BlockSize > INT32_MAX)
      return failure(diagnostic::FileMetadataOption);
    for (auto T : {M.AccessTime, M.ModificationTime, M.ChangeTime, M.BirthTime})
      if (!validTime(T))
        return failure(diagnostic::FileMetadataOption);
  }
  uint64_t DirectoryRecords = 0;
  std::map<std::string, uint64_t> Inodes;
  for (const auto &[Path, Metadata] : Options.Metadata)
    Inodes.emplace(Path, Metadata.Inode);
  for (const auto &[Path, Contents] : Options.DirectoryContents) {
    if (pathKind(Options, Path) != PathKind::Directory)
      return failure(diagnostic::DirectoryContentsOption);
    if (!Options.Directories.contains(Path) &&
        !Options.Metadata.contains(Path)) {
      if (++Entries > limits::Files)
        return failure(diagnostic::FileOptionsLimit);
      if (auto E = PathInput(Path, true))
        return E;
    }
    if (Contents.MinimumBufferSize == 0 ||
        Contents.MinimumBufferSize > DirectoryPayloadLimit)
      return failure(diagnostic::DirectoryContentsOption);
    if (Contents.Entries.size() > limits::DirectoryEntries - DirectoryRecords)
      return failure(diagnostic::FileOptionsLimit);
    DirectoryRecords += Contents.Entries.size();
    const std::string Prefix = Path == "/" ? "/" : Path + '/';
    std::set<std::string> Children = {".", ".."};
    auto Child = [&](llvm::StringRef Other) {
      if (Other.consume_front(Prefix) && !Other.empty())
        Children.insert(Other.take_front(Other.find('/')).str());
    };
    for (const auto &[File, Bytes] : Options.Files)
      Child(File);
    for (const auto &Directory : Options.Directories)
      Child(Directory);
    std::set<uint64_t> Cookies;
    for (const auto &Entry : Contents.Entries) {
      if (Entry.Name.empty() || Entry.Name.size() > limits::Name ||
          llvm::StringRef(Entry.Name).contains('/') ||
          llvm::StringRef(Entry.Name).contains('\0') ||
          !Children.erase(Entry.Name) || !Entry.Inode || !Entry.NextOffset ||
          Entry.NextOffset > INT64_MAX ||
          !Cookies.insert(Entry.NextOffset).second ||
          Entry.MinimumBufferSize > DirectoryPayloadLimit)
        return failure(diagnostic::DirectoryContentsOption);
      std::string Target = Entry.Name == "." ? Path : Prefix + Entry.Name;
      if (Entry.Name == "..")
        Target = Path.substr(0, std::max<size_t>(1, Path.rfind('/')));
      const auto Kind = pathKind(Options, Target);
      const uint8_t Type = Kind == PathKind::File ? 8 : 4;
      if (Kind == PathKind::Missing || (Entry.Type && Entry.Type != Type))
        return failure(diagnostic::DirectoryContentsOption);
      auto [Known, Inserted] = Inodes.emplace(Target, Entry.Inode);
      if (!Inserted && Known->second != Entry.Inode)
        return failure(diagnostic::DirectoryContentsOption);
      const uint64_t Cost = directoryRecordSize(Entry.Name.size());
      if (Cost > limits::Bytes - Total)
        return failure(diagnostic::FileOptionsLimit);
      Total += Cost;
    }
    if (!Children.empty())
      return failure(diagnostic::DirectoryContentsOption);
  }
  if (Options.WorkingDirectory) {
    const auto &Path = *Options.WorkingDirectory;
    if (pathKind(Options, Path) != PathKind::Directory)
      return failure(diagnostic::FileWorkingDirectoryOption);
    if (auto E = PathInput(Path, true))
      return E;
  }
  auto HasAlias = [&](const std::string &Path) {
    auto Inode = Inodes.find(Path);
    if (Inode == Inodes.end())
      return false;
    for (const auto &[Other, Number] : Inodes) {
      if (Other == Path || Number != Inode->second)
        continue;
      auto A = Options.Metadata.find(Path), B = Options.Metadata.find(Other);
      if (A == Options.Metadata.end() || B == Options.Metadata.end() ||
          A->second.Device == B->second.Device)
        return true;
    }
    return false;
  };
  for (const auto &Path : Options.MutableDirectories) {
    if (pathKind(Options, Path) != PathKind::Directory)
      return failure(diagnostic::DirectoryMutableOption);
    if (!Options.Directories.contains(Path) &&
        !Options.Metadata.contains(Path) &&
        !Options.DirectoryContents.contains(Path) && ++Entries > limits::Files)
      return failure(diagnostic::FileOptionsLimit);
    if (auto E = PathInput(Path, true))
      return E;
    const auto M = Options.Metadata.find(Path);
    if (M != Options.Metadata.end() &&
        (M->second.Flags || (M->second.Mode & 07000)))
      return failure(diagnostic::NamespaceFlags);
    if (HasAlias(Path))
      return failure(diagnostic::NamespaceAlias);
  }
  for (const auto &[Path, Bytes] : Options.Files) {
    if (!Options.MutableDirectories.contains(parentPath(Path)))
      continue;
    const auto M = Options.Metadata.find(Path);
    if (M != Options.Metadata.end() && M->second.Flags)
      return failure(diagnostic::NamespaceFlags);
    if (HasAlias(Path) ||
        (M != Options.Metadata.end() && M->second.LinkCount != 1))
      return failure(diagnostic::NamespaceAlias);
  }
  for (const auto &Path : Options.WritableFiles) {
    if (!Options.Files.contains(Path))
      return failure(diagnostic::FileWritableOption);
    if (auto E = PathInput(Path, false))
      return E;
    auto Metadata = Options.Metadata.find(Path);
    if (Metadata != Options.Metadata.end() &&
        (Metadata->second.Flags & FileRestrictedFlags))
      return failure(diagnostic::FileWritableFlags);
    if (HasAlias(Path))
      return failure(diagnostic::FileWritableAlias);
  }
  for (const auto &[Path, Policy] : Options.MutationPolicies) {
    const auto Metadata = Options.Metadata.find(Path);
    const uint64_t Unit = Policy.AllocationUnit;
    if (!Options.WritableFiles.contains(Path) ||
        Metadata == Options.Metadata.end() || !validMutationPolicy(Policy))
      return failure(diagnostic::FileMutationPolicy);
    const auto &M = Metadata->second;
    if ((M.Mode & ~0777) != FileRegularMode || M.Flags || M.LinkCount != 1 ||
        M.Blocks != ((M.Size + Unit - 1) / Unit) * (Unit / 512))
      return failure(diagnostic::FileMutationPolicy);
    if (auto E = PathInput(Path, false))
      return E;
  }
  if (Options.CreationPolicy) {
    const auto &Policy = *Options.CreationPolicy;
    if (Options.MutableDirectories.empty() || !Policy.FirstInode ||
        !Policy.BlockSize || Policy.BlockSize > INT32_MAX ||
        !Options.InitialUmask || !validTime(Policy.Time) ||
        !validMutationPolicy(Policy.Mutation) ||
        llvm::any_of(Inodes, [&](const auto &I) {
          return I.second >= Policy.FirstInode;
        }))
      return failure(diagnostic::FileCreationPolicy);
    for (const auto &Path : Options.MutableDirectories)
      if (!Options.Metadata.contains(Path))
        return failure(diagnostic::FileCreationParent);
  }
  return FileFootprint{Total, uint32_t(Entries)};
}
} // namespace

llvm::Error validateFileOptions(const DarwinFileOptions &Options) {
  auto Size = fileOptionsFootprint(Options);
  return Size ? llvm::Error::success() : Size.takeError();
}

DarwinFiles::DarwinFiles(GuestMemory &Memory,
                         const std::optional<DarwinFileOptions> &Options)
    : Memory(Memory), Options(Options),
      CurrentDirectory(Options ? Options->WorkingDirectory : std::nullopt) {
  const llvm::ArrayRef<uint8_t> Input =
      Options && Options->StandardInput
          ? llvm::ArrayRef<uint8_t>(*Options->StandardInput)
          : llvm::ArrayRef<uint8_t>();
  Descriptors.emplace(0, Descriptor{std::make_shared<Description>(
                             Description{Kind::Input, Input})});
  Descriptors.emplace(1, Descriptor{std::make_shared<Description>(
                             Description{Kind::Output, {}})});
  Descriptors.emplace(2, Descriptor{std::make_shared<Description>(
                             Description{Kind::Error, {}})});
  Descriptors.at(1).Open->Flags = Descriptors.at(2).Open->Flags = OpenWriteOnly;
}
uint32_t DarwinFiles::limit() const {
  return Options ? Options->DescriptorLimit : limits::DefaultDescriptors;
}
uint32_t DarwinFiles::freeDescriptor(uint32_t Minimum) const {
  while (Minimum < limit() && Descriptors.contains(Minimum))
    ++Minimum;
  return Minimum;
}
std::optional<unsigned> DarwinFiles::outputSink(uint32_t FD) const {
  auto I = Descriptors.find(FD);
  if (I != Descriptors.end()) {
    if (I->second.Open->Type == Kind::Output)
      return 1;
    if (I->second.Open->Type == Kind::Error)
      return 2;
  }
  return std::nullopt;
}
void DarwinFiles::recordWrite(uint32_t FD) {
  Descriptors.at(FD).Open->Flags |= FileWasWritten;
}

void DarwinFiles::initializeNamespace() {
  if (NamespaceReady)
    return;
  if (Options->CreationPolicy) {
    NextCreatedInode = Options->CreationPolicy->FirstInode;
  }
  if (Options->InitialUmask)
    CurrentUmask = *Options->InitialUmask;
  for (const auto &[Path, Bytes] : Options->Files) {
    auto Node = std::make_shared<Contents>();
    Node->Initial = Bytes;
    Node->Writable = Options->WritableFiles.contains(Path);
    const auto Metadata = Options->Metadata.find(Path);
    if (Metadata != Options->Metadata.end())
      Node->InitialMetadata = &Metadata->second;
    const auto Policy = Options->MutationPolicies.find(Path);
    if (Policy != Options->MutationPolicies.end())
      Node->Policy = &Policy->second;
    Nodes.emplace(Path, std::move(Node));
  }
  NamespaceReady = true;
}

void DarwinFiles::reclaimUnlinked() {
  for (auto I = Unlinked.begin(); I != Unlinked.end();) {
    if (I->use_count() == 1 && (*I)->Lease.use_count() == 1) {
      *StorageUsed -= (*I)->bytes().size() + (*I)->PathCharge;
      I = Unlinked.erase(I);
    } else {
      ++I;
    }
  }
}

llvm::Error DarwinFiles::prepareMutation() {
  if (!StorageUsed) {
    auto Footprint = fileOptionsFootprint(*Options);
    if (!Footprint)
      return Footprint.takeError();
    StorageUsed = Footprint->Bytes;
    FixedEntries = Footprint->Entries - Options->Files.size();
  }
  reclaimUnlinked();
  return llvm::Error::success();
}

llvm::Expected<DarwinFiles::Pathname> DarwinFiles::readPath(uint64_t Address) {
  std::string Path;
  for (uint64_t I = 0; I < limits::Path; ++I) {
    if (Address >= UserLimit || I >= UserLimit - Address)
      return uint32_t(BadAddress);
    auto Access = Memory.canAccess(Address + I, 1, Read | UserAccessible);
    if (!Access)
      return Access.takeError();
    if (!*Access)
      return uint32_t(BadAddress);
    auto Byte = Memory.readInteger(Address + I, 1);
    if (!Byte)
      return Byte.takeError();
    if (!*Byte)
      return Path;
    Path.push_back(*Byte);
  }
  return uint32_t(NameTooLong);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::open(uint64_t Address, uint32_t Flags, uint32_t DirectoryFD,
                  uint32_t Mode, ProcessResult &Result) {
  if (!Options)
    return unsupported(Result, diagnostic::FileInputs);
  if ((Flags & OpenAccessMask) == OpenAccessMask)
    return returned(InvalidArgument, true);
  if (Flags & ~uint32_t(OpenCloseOnExec | OpenDirectory | OpenAccessMask |
                        OpenAppend | OpenTruncate | OpenCreate | OpenExclusive))
    return unsupported(Result, diagnostic::FileOpenFlags);
  // XNU reserves the descriptor before resolving the pathname. A failed
  // open does not retain that reservation.
  const uint32_t FD = freeDescriptor();
  if (FD == limit())
    return returned(TooManyFiles, true);
  if ((Flags & OpenCreate) && (Flags & OpenDirectory))
    return returned(InvalidArgument, true);
  auto Resolved = resolvePath(Address, DirectoryFD, Flags & OpenCreate);
  if (!Resolved)
    return Resolved.takeError();
  if (auto *Error = std::get_if<uint32_t>(&*Resolved))
    return returned(*Error, true);
  if (auto *Reason = std::get_if<const char *>(&*Resolved))
    return unsupported(Result, *Reason);
  auto &File = std::get<Description>(*Resolved);
  const bool Created = File.Type == Kind::Missing;
  if (!Created && (Flags & OpenCreate) && (Flags & OpenExclusive))
    return returned(FileExists, true);
  if (Created) {
    auto Made = create(File, Mode, Result);
    if (!Made || !*Made || (**Made).Error)
      return Made;
  }
  if ((Flags & OpenDirectory) && File.Type != Kind::Directory)
    return returned(NotDirectory, true);
  const bool Mutating = (Flags & OpenAccessMask) || (Flags & OpenTruncate);
  if (Mutating && File.Type == Kind::Directory)
    return returned(IsDirectory, true);
  if (Mutating && !File.File->Writable)
    return unsupported(Result, diagnostic::FileNotWritable);
  File.Flags = Flags & (OpenAccessMask | OpenAppend);
  if ((Flags & OpenTruncate) && !Created) {
    auto Truncated = resize(File, 0, Result);
    if (!Truncated || !*Truncated || (**Truncated).Error)
      return Truncated;
    File.Flags |= FileWasWritten;
  }
  Descriptors.emplace(FD, Descriptor{std::make_shared<Description>(File),
                                     bool(Flags & OpenCloseOnExec)});
  return returned(FD);
}

llvm::Expected<DarwinFiles::Lookup>
DarwinFiles::resolvePath(uint64_t Address, uint32_t DirectoryFD,
                         bool AllowMissing) {
  if (!Options)
    return diagnostic::FileInputs;
  initializeNamespace();
  auto Imported = readPath(Address);
  if (!Imported)
    return Imported.takeError();
  if (auto *Error = std::get_if<uint32_t>(&*Imported))
    return *Error;
  const auto &Path = std::get<std::string>(*Imported);
  const bool Absolute = llvm::StringRef(Path).starts_with('/');
  std::string Prefix = "/";
  if (!Absolute) {
    if (DirectoryFD != AtCurrentDirectory) {
      auto FD = Descriptors.find(DirectoryFD);
      if (FD == Descriptors.end())
        return uint32_t(BadDescriptor);
      if (FD->second.Open->Type == Kind::File)
        return uint32_t(NotDirectory);
      if (FD->second.Open->Type != Kind::Directory)
        return diagnostic::FileDirectoryKind;
      Prefix = FD->second.Open->Path;
    } else if (!Path.empty()) {
      if (!CurrentDirectory)
        return diagnostic::FileWorkingDirectory;
      Prefix = *CurrentDirectory;
    }
  }
  if (Path.empty())
    return uint32_t(NoEntry);
  llvm::SmallVector<llvm::StringRef> Parts;
  llvm::StringRef(Path).split(Parts, '/');
  // Walk before reducing dot components. A missing or regular-file ancestor
  // wins over later '..', long names and trailing slashes.
  auto Type = PathKind::Directory;
  for (size_t Index = Absolute; Index < Parts.size(); ++Index) {
    const auto Part = Parts[Index];
    if (Type != PathKind::Directory)
      return uint32_t(NotDirectory);
    if (Part.size() > limits::Name)
      return uint32_t(NameTooLong);
    if (Part.empty() || Part == ".")
      continue;
    if (Part == "..") {
      Prefix.resize(std::max<size_t>(1, Prefix.rfind('/')));
    } else {
      if (Prefix != "/")
        Prefix += '/';
      Prefix.append(Part.data(), Part.size());
    }
    // Initial ancestor directories remain real after their last child name
    // is removed. Regular-file names come only from the live namespace.
    Type = Nodes.contains(Prefix) ? PathKind::File
           : pathKind(*Options, Prefix) == PathKind::Directory
               ? PathKind::Directory
               : PathKind::Missing;
    if (Type == PathKind::Missing) {
      if (AllowMissing && Index + 1 == Parts.size())
        return Description{Kind::Missing, {}, 0, nullptr, std::move(Prefix)};
      return uint32_t(NoEntry);
    }
  }
  auto Metadata = Options->Metadata.find(Prefix);
  Description File{Type == PathKind::File ? Kind::File : Kind::Directory,
                   {},
                   0,
                   Metadata == Options->Metadata.end() ? nullptr
                                                       : &Metadata->second,
                   std::move(Prefix)};
  if (Type == PathKind::File) {
    File.File = Nodes.at(File.Path);
    File.Metadata = nullptr;
  }
  return File;
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::copyout(uint64_t Address, llvm::ArrayRef<uint8_t> Bytes,
                     const char *PartialDiagnostic, ProcessResult &Result) {
  return copyUserMemory(Memory, Address, Bytes, PartialDiagnostic, Result);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::status(const Description &File, uint64_t Address,
                    ProcessResult &Result) {
  if (File.Type == Kind::Directory && ChangedDirectories.contains(File.Path))
    return unsupported(Result, diagnostic::DirectoryMutated);
  if (File.File && File.File->MetadataInvalidated)
    return unsupported(Result, diagnostic::FileMutatedMetadata);
  const auto *Metadata = File.File ? File.File->metadata() : File.Metadata;
  if (!Metadata)
    return unsupported(Result, diagnostic::FileMetadata);
  std::array<uint8_t, FileStatusSize> Bytes{};
  auto Put = [&](unsigned Offset, unsigned Width, uint64_t Value) {
    if (Width == 2)
      llvm::support::endian::write16le(Bytes.data() + Offset, Value);
    else if (Width == 4)
      llvm::support::endian::write32le(Bytes.data() + Offset, Value);
    else
      llvm::support::endian::write64le(Bytes.data() + Offset, Value);
  };
#define NEVERD_DARWIN_FILE_STATUS(Member, Native, Offset, Width)               \
  Put(Offset, Width, static_cast<uint64_t>(Metadata->Member));
#include "DarwinFileStatus.def"
#undef NEVERD_DARWIN_FILE_STATUS
  return copyout(Address, Bytes, diagnostic::FilePartialStatus, Result);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::statusPath(uint64_t Path, uint64_t Address, uint32_t DirectoryFD,
                        ProcessResult &Result) {
  auto Resolved = resolvePath(Path, DirectoryFD);
  if (!Resolved)
    return Resolved.takeError();
  if (auto *Error = std::get_if<uint32_t>(&*Resolved))
    return returned(*Error, true);
  if (auto *Reason = std::get_if<const char *>(&*Resolved))
    return unsupported(Result, *Reason);
  return status(std::get<Description>(*Resolved), Address, Result);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::unlink(uint64_t Path, uint32_t DirectoryFD,
                    ProcessResult &Result) {
  auto Resolved = resolvePath(Path, DirectoryFD);
  if (!Resolved)
    return Resolved.takeError();
  if (auto *Error = std::get_if<uint32_t>(&*Resolved))
    return returned(*Error, true);
  if (auto *Reason = std::get_if<const char *>(&*Resolved))
    return unsupported(Result, *Reason);
  auto &File = std::get<Description>(*Resolved);
  if (File.Path == "/")
    return returned(ResourceBusy, true);
  if (File.Type == Kind::Directory)
    return returned(OperationNotPermitted, true);
  const auto Parent = parentPath(File.Path);
  if (!Options->MutableDirectories.contains(Parent))
    return unsupported(Result, diagnostic::DirectoryNotMutable);
  if (auto E = prepareMutation())
    return std::move(E);
  auto &Node = *File.File;
  if (Node.Policy && !Node.MetadataInvalidated) {
    if (!Node.CurrentMetadata)
      Node.CurrentMetadata = *Node.InitialMetadata;
    Node.CurrentMetadata->LinkCount = 0;
    Node.CurrentMetadata->ChangeTime = Node.Policy->Time;
  } else {
    Node.MetadataInvalidated = true;
  }
  ChangedDirectories.insert(Parent);
  Unlinked.push_back(File.File);
  Nodes.erase(File.Path);
  return returned(0);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::create(Description &File, uint32_t Mode, ProcessResult &Result) {
  const auto Parent = parentPath(File.Path);
  if (!Options->MutableDirectories.contains(Parent))
    return unsupported(Result, diagnostic::DirectoryNotMutable);
  if (auto E = prepareMutation())
    return std::move(E);
  const uint64_t Charge = File.Path.size() + 1;
  if (File.Path.size() >= limits::Path ||
      FixedEntries + Nodes.size() + Unlinked.size() >= limits::Files ||
      Charge > limits::Bytes - *StorageUsed)
    return unsupported(Result, diagnostic::FileCreationLimit);
  if (Options->CreationPolicy && !NextCreatedInode)
    return unsupported(Result, diagnostic::FileCreationInode);
  auto Node = std::make_shared<Contents>();
  Node->Writable = true;
  Node->PathCharge = Charge;
  if (Options->CreationPolicy) {
    const auto &Policy = *Options->CreationPolicy;
    // Namespace changes invalidate the parent's complete stat observation,
    // but cannot change these supplied device/group fields.
    const auto &ParentMetadata = Options->Metadata.at(Parent);
    auto &M = Node->CurrentMetadata.emplace();
    M.Device = ParentMetadata.Device;
    M.GID = ParentMetadata.GID;
    M.UID = UserID;
    M.Inode = NextCreatedInode;
    M.Mode = FileRegularMode | (Mode & 0777 & ~CurrentUmask);
    M.LinkCount = 1;
    M.BlockSize = Policy.BlockSize;
    M.Generation = Policy.Generation;
    M.AccessTime = M.ModificationTime = M.ChangeTime = M.BirthTime =
        Policy.Time;
    Node->Policy = &Policy.Mutation;
  }
  // A new object never inherits an old observation/policy at the same name.
  File.Type = Kind::File;
  File.File = Node;
  *StorageUsed += Charge;
  Nodes.emplace(File.Path, std::move(Node));
  ChangedDirectories.insert(Parent);
  if (Options->CreationPolicy)
    NextCreatedInode =
        NextCreatedInode == UINT64_MAX ? 0 : NextCreatedInode + 1;
  return returned(0);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::read(Description &File, uint64_t Address, uint64_t Count,
                  uint64_t Offset, bool Positioned, ProcessResult &Result) {
  if ((File.Flags & OpenAccessMask) == OpenWriteOnly)
    return returned(BadDescriptor, true);
  if (Positioned && File.Type == Kind::Input)
    return returned(IllegalSeek, true);
  if (Offset > INT64_MAX)
    return returned(InvalidArgument, true);
  if (File.Type == Kind::Directory)
    return returned(IsDirectory, true);
  if (Count && File.Type == Kind::Input &&
      (!Options || !Options->StandardInput))
    return unsupported(Result, diagnostic::FileInput);
  const auto Bytes = File.bytes();
  const uint64_t Available =
      Bytes.size() - std::min<uint64_t>(Offset, Bytes.size());
  Count = std::min(Count, Available);
  if (!Count)
    return returned(0);
  auto Stored = copyout(Address, Bytes.slice(Offset, Count),
                        diagnostic::FilePartialRead, Result);
  if (!Stored)
    return Stored.takeError();
  if (!*Stored || (**Stored).Error)
    return Stored;
  if (!Positioned)
    File.Offset += Count;
  return returned(Count);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::admitMutation(const Description &File, uint64_t Size,
                           ProcessResult &Result) {
  if (!File.File->Writable)
    return unsupported(Result, diagnostic::FileNotWritable);
  if (File.File->Lease.use_count() != 1)
    return unsupported(Result, diagnostic::FileMutationMapping);
  if (auto E = prepareMutation())
    return std::move(E);
  const uint64_t Other = *StorageUsed - File.bytes().size();
  if (Size > limits::Bytes - Other)
    return unsupported(Result, diagnostic::FileMutationLimit);
  return returned(0);
}

void DarwinFiles::publish(
    Description &File, std::vector<uint8_t> Bytes,
    std::optional<std::pair<uint64_t, uint64_t>> Written) {
  *StorageUsed = *StorageUsed - File.bytes().size() + Bytes.size();
  auto &Node = *File.File;
  Node.Modified = std::move(Bytes);
  if (!Node.Policy || Node.MetadataInvalidated) {
    Node.MetadataInvalidated = true;
    return;
  }
  // This allocation rule is opted into explicitly, never inferred from the
  // observed st_blksize or from which input bytes happen to be zero.
  const uint64_t Unit = Node.Policy->AllocationUnit;
  auto Units = [Unit](uint64_t Size) { return (Size + Unit - 1) / Unit; };
  if (!Node.Allocated) {
    Node.Allocated.emplace(Units(Node.Initial.size()), true);
  }
  if (!Node.CurrentMetadata)
    Node.CurrentMetadata = *Node.InitialMetadata;
  Node.Allocated->resize(Units(Node.bytes().size()), false);
  if (Written)
    Node.Allocated->set(Written->first / Unit,
                        Units(Written->first + Written->second));
  auto &M = *Node.CurrentMetadata;
  M.Size = Node.bytes().size();
  M.Blocks = Node.Allocated->count() * (Unit / 512);
  M.ModificationTime = M.ChangeTime = Node.Policy->Time;
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::resize(Description &File, uint64_t Size, ProcessResult &Result) {
  auto Admitted = admitMutation(File, Size, Result);
  if (!Admitted || !*Admitted)
    return Admitted;
  // Replace storage even when shrinking, so reclaimed logical capacity cannot
  // accumulate in retained vector allocations across successive files.
  std::vector<uint8_t> Bytes(Size, 0);
  const auto Previous = File.bytes();
  std::copy_n(Previous.begin(), std::min<uint64_t>(Size, Previous.size()),
              Bytes.begin());
  publish(File, std::move(Bytes));
  return returned(0);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::write(Description &File, uint64_t Address, uint64_t Count,
                   uint64_t Offset, bool Positioned, ProcessResult &Result) {
  if (!(File.Flags & OpenAccessMask))
    return returned(BadDescriptor, true);
  if (File.Type != Kind::File)
    return returned(IllegalSeek, true);
  if (Offset > INT64_MAX)
    return returned(InvalidArgument, true);
  // vn_write checks and clips the supplied cursor before the filesystem
  // chooses EOF for append, including the count-zero EFBIG boundary.
  if (Offset == INT64_MAX)
    return returned(FileTooLarge, true);
  Count = std::min(Count, uint64_t(INT64_MAX) - Offset);
  if (!Count)
    return returned(0);
  const bool Append = !Positioned && (File.Flags & OpenAppend);
  if (Append)
    Offset = File.bytes().size();
  const uint64_t Size = std::max<uint64_t>(File.bytes().size(), Offset + Count);
  auto Admitted = admitMutation(File, Size, Result);
  if (!Admitted || !*Admitted)
    return Admitted;
  uint64_t Checked = 0;
  while (Checked < Count) {
    const uint64_t Start = Address + Checked;
    const uint64_t Chunk = std::min(Count - Checked, 4096 - Start % 4096);
    auto Access = Start < UserLimit
                      ? Memory.canAccess(Start, Chunk, Read | UserAccessible)
                      : llvm::Expected<bool>(false);
    if (!Access)
      return Access.takeError();
    if (!*Access) {
      if (Checked)
        return unsupported(Result, diagnostic::FilePartialWrite);
      // A wholly invalid input leaves contents unchanged. Darwin still
      // publishes the chosen EOF cursor for a nonempty append request.
      if (Append)
        File.Offset = Offset;
      // Filesystems can update mtime/ctime even after rolling back a failed
      // extension. Do not retain a complete pre-write stat observation.
      File.File->MetadataInvalidated = true;
      return returned(BadAddress, true);
    }
    Checked += Chunk;
  }
  std::vector<uint8_t> Bytes(Size, 0);
  const auto Previous = File.bytes();
  std::copy(Previous.begin(), Previous.end(), Bytes.begin());
  if (auto E = Memory.read(
          Address, llvm::MutableArrayRef<uint8_t>(Bytes).slice(Offset, Count)))
    return std::move(E);
  publish(File, std::move(Bytes), std::pair{Offset, Count});
  if (!Positioned)
    File.Offset = Offset + Count;
  File.Flags |= FileWasWritten;
  return returned(Count);
}

ServiceResult DarwinFiles::seek(Description &File, uint64_t Offset,
                                uint32_t Whence) {
  if (File.Type != Kind::File && File.Type != Kind::Directory)
    return {IllegalSeek, true};
  if (Whence == SeekHole || Whence == SeekData) {
    if (Offset > INT64_MAX)
      return {InvalidArgument, true};
    const uint64_t Size = File.bytes().size();
    if (Offset >= Size)
      return {NoSuchAddress, true};
    const auto &Node = *File.File;
    const bool Data = Whence == SeekData;
    uint64_t Position = Data ? Offset : Size;
    // Before the first mutation the admitted policy asserts dense allocation.
    // Afterward, consult the same ledger used for st_blocks; zero bytes alone
    // do not describe a hole.
    if (Node.Allocated) {
      const uint64_t Unit = Node.Policy->AllocationUnit;
      const unsigned Index = Offset / Unit;
      if (Node.Allocated->test(Index) == Data) {
        Position = Offset;
      } else {
        const int Next = Data ? Node.Allocated->find_next(Index)
                              : Node.Allocated->find_next_unset(Index);
        if (Next < 0 && Data)
          return {NoSuchAddress, true};
        Position = Next < 0 ? Size : std::min(Size, uint64_t(Next) * Unit);
      }
    }
    File.Offset = Position;
    return {Position, false};
  }
  uint64_t Base;
  switch (Whence) {
  case 0:
    Base = 0;
    break;
  case 1:
    Base = File.Offset;
    break;
  case 2:
    Base = File.Type == Kind::Directory ? File.Metadata->Size
                                        : File.bytes().size();
    break;
  default:
    return {InvalidArgument, true};
  }
  if (Offset <= INT64_MAX) {
    if (Offset > uint64_t(INT64_MAX) - Base)
      return {Overflow, true};
    Base += Offset;
  } else {
    const uint64_t Magnitude = uint64_t(0) - Offset;
    if (Magnitude > Base)
      return {InvalidArgument, true};
    Base -= Magnitude;
  }
  File.Offset = Base;
  return {Base, false};
}

DarwinFiles::MappingSource DarwinFiles::mappingSource(uint32_t FD) const {
  auto I = Descriptors.find(FD);
  if (I == Descriptors.end())
    return uint32_t(BadDescriptor);
  if (I->second.Open->Type == Kind::Directory)
    return uint32_t(InvalidArgument);
  if (I->second.Open->Type != Kind::File)
    return diagnostic::MemoryFileKind;
  const auto &File = *I->second.Open->File;
  return Mapping{File.bytes(), File.Lease,
                 (I->second.Open->Flags & OpenAccessMask) != OpenWriteOnly};
}

ServiceResult DarwinFiles::duplicate(const Descriptor &Source, uint32_t Minimum,
                                     bool CloseOnExec) {
  const uint32_t FD = freeDescriptor(Minimum);
  if (FD >= limit())
    return {TooManyFiles, true};
  Descriptors.emplace(FD, Descriptor{Source.Open, CloseOnExec});
  return {FD, false};
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::handle(ServiceKind Service, const ProcessServiceEvent &Event,
                    ProcessResult &Result) {
  const auto &A = Event.Arguments;
  if (Service == ServiceKind::Umask) {
    if (!Options || !Options->InitialUmask)
      return unsupported(Result, diagnostic::FileUmask);
    initializeNamespace();
    const auto Previous = CurrentUmask;
    CurrentUmask = A[0] & 07777;
    return returned(Previous);
  }
  if (Service == ServiceKind::Open)
    return open(A[0], A[1], AtCurrentDirectory, A[2], Result);
  if (Service == ServiceKind::OpenAt)
    return open(A[1], A[2], A[0], A[3], Result);
  if (Service == ServiceKind::Unlink)
    return unlink(A[0], AtCurrentDirectory, Result);
  if (Service == ServiceKind::UnlinkAt) {
    const uint32_t Flags = A[2];
    if (Flags & ~uint32_t(AtRemoveDirectory | AtRemoveDatalessDirectory |
                          AtNoFollowAny | AtSystemDiscarded))
      return returned(InvalidArgument, true);
    if (Flags & ~uint32_t(AtNoFollowAny))
      return unsupported(Result, diagnostic::UnlinkFlags);
    return unlink(A[1], A[0], Result);
  }
  if (Service == ServiceKind::Truncate || Service == ServiceKind::Ftruncate) {
    if (A[1] > INT64_MAX)
      return returned(InvalidArgument, true);
    if (Service == ServiceKind::Truncate) {
      auto Resolved = resolvePath(A[0], AtCurrentDirectory);
      if (!Resolved)
        return Resolved.takeError();
      if (auto *Error = std::get_if<uint32_t>(&*Resolved))
        return returned(*Error, true);
      if (auto *Reason = std::get_if<const char *>(&*Resolved))
        return unsupported(Result, *Reason);
      auto &File = std::get<Description>(*Resolved);
      if (File.Type == Kind::Directory)
        return returned(IsDirectory, true);
      return resize(File, A[1], Result);
    }
  }
  if (Service == ServiceKind::Stat64 || Service == ServiceKind::Lstat64)
    return statusPath(A[0], A[1], AtCurrentDirectory, Result);
  if (Service == ServiceKind::FstatAt64) {
    const uint32_t Flags = A[3];
    if (Flags & ~uint32_t(AtNoFollow | AtNoFollowAny | AtFDOnly | AtRealDevice))
      return returned(InvalidArgument, true);
    if (Flags & AtRealDevice)
      return unsupported(Result, diagnostic::FileStatFlags);
    if (!(Flags & AtFDOnly))
      return statusPath(A[1], A[2], A[0], Result);
    // AT_FDONLY ignores the pathname completely, including invalid pointers.
  }
  if (Service == ServiceKind::Chdir) {
    auto Resolved = resolvePath(A[0], AtCurrentDirectory);
    if (!Resolved)
      return Resolved.takeError();
    if (auto *Error = std::get_if<uint32_t>(&*Resolved))
      return returned(*Error, true);
    if (auto *Reason = std::get_if<const char *>(&*Resolved))
      return unsupported(Result, *Reason);
    const auto &Directory = std::get<Description>(*Resolved);
    if (Directory.Type != Kind::Directory)
      return returned(NotDirectory, true);
    CurrentDirectory = Directory.Path;
    return returned(0);
  }
  if (Service == ServiceKind::Pwrite && A[3] == UINT64_MAX)
    return returned(InvalidArgument, true);
  if ((Service == ServiceKind::Read || Service == ServiceKind::Pread ||
       Service == ServiceKind::Write || Service == ServiceKind::Pwrite) &&
      A[2] > MaxWriteBytes)
    return returned(InvalidArgument, true);
  auto I = Descriptors.find(uint32_t(A[0]));
  if (I == Descriptors.end())
    return returned(BadDescriptor, true);
  auto &FD = I->second;
  auto &File = *FD.Open;
  switch (Service) {
  case ServiceKind::Write:
  case ServiceKind::Pwrite:
    return write(File, A[1], A[2],
                 Service == ServiceKind::Pwrite ? A[3] : File.Offset,
                 Service == ServiceKind::Pwrite, Result);
  case ServiceKind::Ftruncate: {
    if (File.Type != Kind::File || !(File.Flags & OpenAccessMask))
      return returned(InvalidArgument, true);
    auto Truncated = resize(File, A[1], Result);
    if (Truncated && *Truncated && !(**Truncated).Error)
      File.Flags |= FileWasWritten;
    return Truncated;
  }
  case ServiceKind::Fstat64:
    return status(File, A[1], Result);
  case ServiceKind::GetDirEntries64:
    return directory(File, A[1], A[2], A[3], Result);
  case ServiceKind::FstatAt64:
    return status(File, A[2], Result);
  case ServiceKind::Fchdir:
    if (File.Type == Kind::File)
      return returned(NotDirectory, true);
    if (File.Type != Kind::Directory)
      return unsupported(Result, diagnostic::FileDirectoryKind);
    CurrentDirectory = File.Path;
    return returned(0);
  case ServiceKind::Read:
  case ServiceKind::Pread:
    return read(File, A[1], A[2],
                Service == ServiceKind::Pread ? A[3] : File.Offset,
                Service == ServiceKind::Pread, Result);
  case ServiceKind::Close:
    Descriptors.erase(I);
    reclaimUnlinked();
    return returned(0);
  case ServiceKind::Lseek:
    if (File.Type == Kind::File || File.Type == Kind::Directory) {
      if (uint32_t(A[2]) == SeekHole || uint32_t(A[2]) == SeekData)
        if (File.Type != Kind::File || !File.File->Policy ||
            File.File->MetadataInvalidated)
          return unsupported(Result, diagnostic::FileSeek);
      if (File.Type == Kind::Directory && uint32_t(A[2]) == 2 && !File.Metadata)
        return unsupported(Result, diagnostic::FileMetadata);
      if (File.Type == Kind::Directory && uint32_t(A[2]) == 2 &&
          ChangedDirectories.contains(File.Path))
        return unsupported(Result, diagnostic::DirectoryMutated);
    }
    return std::optional<ServiceResult>(seek(File, A[1], A[2]));
  case ServiceKind::Dup:
    return std::optional<ServiceResult>(duplicate(FD, 0, false));
  case ServiceKind::Dup2: {
    const uint32_t Target = A[1];
    if (Target >= limit())
      return returned(BadDescriptor, true);
    if (Target != I->first)
      Descriptors.insert_or_assign(Target, Descriptor{FD.Open, false});
    reclaimUnlinked();
    return returned(Target);
  }
  case ServiceKind::Fcntl:
    switch (uint32_t(A[1])) {
    case DuplicateFD:
    case DuplicateCloseOnExec:
      if (uint32_t(A[2]) >= limit())
        return returned(InvalidArgument, true);
      return std::optional<ServiceResult>(
          duplicate(FD, A[2], uint32_t(A[1]) == DuplicateCloseOnExec));
    case GetDescriptorFlags:
      return returned(FD.CloseOnExec ? 1 : 0);
    case SetDescriptorFlags:
      FD.CloseOnExec = A[2] & 1;
      return returned(0);
    case GetPath: {
      if (File.Path.empty())
        return unsupported(Result, diagnostic::FilePathIdentity);
      return copyout(A[2],
                     llvm::ArrayRef<uint8_t>(
                         reinterpret_cast<const uint8_t *>(File.Path.c_str()),
                         File.Path.size() + 1),
                     diagnostic::FilePartialPath, Result);
    }
    case GetFileFlags:
      return returned(File.Flags);
    case SetFileFlags:
      if (uint32_t(A[2]) &
          ~uint32_t(OpenAccessMask | OpenAppend | FileWasWritten))
        return unsupported(Result, diagnostic::FileControl);
      File.Flags = (File.Flags & ~OpenAppend) | (A[2] & OpenAppend);
      return returned(0);
    default:
      return unsupported(Result, diagnostic::FileControl);
    }
  default:
    llvm_unreachable("non-file Darwin service");
  }
}
} // namespace neverd::emulation::darwin_model
