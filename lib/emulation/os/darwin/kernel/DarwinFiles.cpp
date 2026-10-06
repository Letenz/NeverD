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
  for (const auto &Path : Options.SwapRenameDirectories) {
    if (!Options.Directories.contains(Path) ||
        !Options.MutableDirectories.contains(Path))
      return failure(diagnostic::DirectorySwapOption);
    if (auto E = PathInput(Path, true))
      return E;
  }
  for (const auto &Path : Options.RemovableDirectories) {
    const auto Parent = parentPath(Path);
    if (Path == "/" || !Options.Directories.contains(Path) ||
        !Options.MutableDirectories.contains(Parent))
      return failure(diagnostic::DirectoryRemovableOption);
    if (auto E = PathInput(Path, true))
      return E;
    const auto M = Options.Metadata.find(Path);
    if (M != Options.Metadata.end() &&
        (M->second.Flags || (M->second.Mode & 07000)))
      return failure(diagnostic::NamespaceFlags);
    if (HasAlias(Path))
      return failure(diagnostic::NamespaceAlias);
    const auto P = Options.Metadata.find(Parent);
    if (M != Options.Metadata.end() && P != Options.Metadata.end() &&
        M->second.Device != P->second.Device)
      return failure(diagnostic::DirectoryRemovalDevice);
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
                         const std::optional<DarwinFileOptions> &Options,
                         uint64_t OutputLimit)
    : Memory(Memory), Options(Options), OutputLimit(OutputLimit) {
  if (Options && Options->WorkingDirectory)
    CurrentDirectory = directoryNode(*Options->WorkingDirectory);
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
    Node->Path = Path;
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
  // A removed child retains its original parent. Releasing a leaf can make
  // earlier parents unreachable, so reclaim the bounded chain to a fixed point.
  bool Reclaimed;
  do {
    Reclaimed = false;
    for (auto I = UnlinkedDirectories.begin();
         I != UnlinkedDirectories.end();) {
      if (I->use_count() == 1) {
        *StorageUsed -= (*I)->Path.size() + 1;
        I = UnlinkedDirectories.erase(I);
        Reclaimed = true;
      } else {
        ++I;
      }
    }
  } while (Reclaimed);
}

std::shared_ptr<DarwinFiles::DirectoryNode>
DarwinFiles::directoryNode(const std::string &Path) {
  if (auto I = CreatedDirectories.find(Path); I != CreatedDirectories.end())
    return I->second;
  auto Node = initialDirectoryNode(Path);
  return Node && Node->Linked ? Node : nullptr;
}

std::shared_ptr<DarwinFiles::DirectoryNode>
DarwinFiles::initialDirectoryNode(const std::string &Path) {
  if (auto I = InitialDirectories.find(Path); I != InitialDirectories.end())
    return I->second;
  if (pathKind(*Options, Path) != PathKind::Directory)
    return nullptr;
  auto Node = std::make_shared<DirectoryNode>();
  Node->Path = Path;
  if (Path != "/")
    Node->Parent = initialDirectoryNode(parentPath(Path));
  InitialDirectories.emplace(Path, Node);
  return Node;
}

bool DarwinFiles::hasInitialDirectoryChild(const std::string &Path) {
  const auto Prefix = Path + '/';
  auto Present = [&](llvm::StringRef Input, bool Regular) {
    if (!Input.consume_front(Prefix))
      return false;
    const auto Slash = Input.find('/');
    if (Regular && Slash == llvm::StringRef::npos)
      return false;
    return bool(directoryNode(Prefix + Input.substr(0, Slash).str()));
  };
  // Initial implicit ancestors survive the removal of their last file.
  // Check the declared graph, not just lazily opened nodes or old snapshots.
  return llvm::any_of(
             Options->Files,
             [&](const auto &Entry) { return Present(Entry.first, true); }) ||
         llvm::any_of(Options->Directories,
                      [&](const auto &Entry) { return Present(Entry, false); });
}

bool DarwinFiles::mutableDirectory(const std::string &Path) const {
  return CreatedDirectories.contains(Path) ||
         Options->MutableDirectories.contains(Path);
}

std::optional<DarwinFiles::DirectoryIdentity>
DarwinFiles::directoryIdentity(const std::string &Path) {
  // A new directory may reuse an unlinked initial file's name. Its inherited
  // identity must shadow that old path's observations, even when unknown.
  const auto Created = CreatedDirectories.find(Path);
  if (Created != CreatedDirectories.end())
    return Created->second->Identity;
  const auto Metadata = Options->Metadata.find(Path);
  if (pathKind(*Options, Path) == PathKind::Directory &&
      Metadata != Options->Metadata.end())
    return DirectoryIdentity{Metadata->second.Device, Metadata->second.GID};
  return std::nullopt;
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
  auto Resolved = resolvePath(Address, DirectoryFD,
                              Flags & OpenCreate ? LookupMode::CreateFile
                                                 : LookupMode::Existing);
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
                         LookupMode Mode) {
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
  auto Directory = directoryNode("/");
  if (!Absolute) {
    if (DirectoryFD != AtCurrentDirectory) {
      auto FD = Descriptors.find(DirectoryFD);
      if (FD == Descriptors.end())
        return uint32_t(BadDescriptor);
      if (FD->second.Open->Type == Kind::File)
        return uint32_t(NotDirectory);
      if (FD->second.Open->Type != Kind::Directory)
        return diagnostic::FileDirectoryKind;
      Directory = FD->second.Open->Directory;
    } else if (!Path.empty()) {
      if (!CurrentDirectory)
        return diagnostic::FileWorkingDirectory;
      Directory = CurrentDirectory;
    }
  }
  if (Path.empty())
    return uint32_t(NoEntry);
  const auto Trimmed = llvm::StringRef(Path).rtrim('/');
  const auto Slash = Trimmed.rfind('/');
  const auto Leaf =
      Slash == llvm::StringRef::npos ? Trimmed : Trimmed.substr(Slash + 1);
  const Terminal FinalComponent = Leaf == "."    ? Terminal::Dot
                                  : Leaf == ".." ? Terminal::DotDot
                                                 : Terminal::Ordinary;
  llvm::SmallVector<llvm::StringRef> Parts;
  llvm::StringRef(Path).split(Parts, '/');
  std::string Prefix = Directory->Path;
  auto Type = PathKind::Directory;
  bool FinalParentUnlinked = false;
  // Walk objects before reducing dots. An unlinked directory keeps its own
  // identity and parent; its last path must never resolve into a reused name.
  for (size_t Index = Absolute; Index < Parts.size(); ++Index) {
    const auto Part = Parts[Index];
    if (Type != PathKind::Directory)
      return uint32_t(NotDirectory);
    if (Part.size() > limits::Name)
      return uint32_t(NameTooLong);
    if (Part.empty())
      continue;
    // RENAME rejects its terminal dot before looking up that component.
    // Earlier missing/file/removed ancestors still take precedence.
    if (Mode == LookupMode::RenameTarget && (Part == "." || Part == "..") &&
        llvm::all_of(
            llvm::ArrayRef<llvm::StringRef>(Parts).drop_front(Index + 1),
            [](llvm::StringRef Part) { return Part.empty(); }))
      return uint32_t(InvalidArgument);
    if (Part == ".")
      continue;
    if (Part == "..") {
      FinalParentUnlinked = !Directory->Linked;
      if (Directory->Parent)
        Directory = Directory->Parent;
      // LOOKUP can follow the retained parent vnode. Namespace operations
      // cannot traverse a removed parent, even to a later dot or ancestor.
      if (Mode != LookupMode::Existing && !Directory->Linked)
        return uint32_t(NoEntry);
      Prefix = Directory->Path;
      continue;
    }
    if (!Directory->Linked)
      return uint32_t(NoEntry);
    Prefix = Directory->Path;
    if (Prefix != "/")
      Prefix += '/';
    Prefix.append(Part.data(), Part.size());
    auto ChildDirectory = directoryNode(Prefix);
    Type = Nodes.contains(Prefix) ? PathKind::File
           : ChildDirectory       ? PathKind::Directory
                                  : PathKind::Missing;
    if (Type == PathKind::Missing) {
      const bool CreatesFile =
          Mode == LookupMode::CreateFile || Mode == LookupMode::RenameTarget;
      const bool CreatesDirectory = Mode == LookupMode::CreateDirectory;
      const bool Final = Index + 1 == Parts.size();
      const bool DirectoryTail =
          CreatesDirectory &&
          llvm::all_of(
              llvm::ArrayRef<llvm::StringRef>(Parts).drop_front(Index + 1),
              [](llvm::StringRef Part) { return Part.empty(); });
      if (((CreatesFile || CreatesDirectory) && Final) || DirectoryTail)
        return Description{Kind::Missing, {}, 0, nullptr, std::move(Prefix)};
      return uint32_t(NoEntry);
    }
    if (ChildDirectory)
      Directory = std::move(ChildDirectory);
    FinalParentUnlinked = false;
  }
  auto Metadata = Options->Metadata.find(Prefix);
  Description File{Type == PathKind::File ? Kind::File : Kind::Directory,
                   {},
                   0,
                   Directory->Created || Metadata == Options->Metadata.end()
                       ? nullptr
                       : &Metadata->second,
                   std::move(Prefix)};
  if (Type == PathKind::File) {
    File.File = Nodes.at(File.Path);
    File.Metadata = nullptr;
  } else {
    File.Directory = std::move(Directory);
  }
  File.FinalComponent = FinalComponent;
  File.FinalParentUnlinked =
      FinalComponent == Terminal::DotDot && FinalParentUnlinked;
  return File;
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::access(uint64_t Path, uint32_t DirectoryFD, uint32_t Mode,
                    ProcessResult &Result) {
  auto Resolved = resolvePath(Path, DirectoryFD);
  if (!Resolved)
    return Resolved.takeError();
  if (auto *Error = std::get_if<uint32_t>(&*Resolved))
    return returned(*Error, true);
  if (auto *Reason = std::get_if<const char *>(&*Resolved))
    return unsupported(Result, *Reason);
  // Native access1 requests authorization only for R/W/X or extended access
  // bits. Other low-carrier bits are ignored, not an EINVAL or a grant. The
  // catalogue proves name existence; stat observations do not prove ACL/MAC
  // authorization, including when mutation grants permit model operations.
  if (Mode & AccessPermissionMask)
    return unsupported(Result, diagnostic::FileAccessPermissions);
  return returned(0);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::copyout(uint64_t Address, llvm::ArrayRef<uint8_t> Bytes,
                     const char *PartialDiagnostic, ProcessResult &Result) {
  return copyUserMemory(Memory, Address, Bytes, PartialDiagnostic, Result);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::status(const Description &File, uint64_t Address,
                    ProcessResult &Result) {
  if (File.Type == Kind::Directory && File.Directory->Changed)
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
DarwinFiles::makeDirectory(uint64_t Path, uint32_t DirectoryFD,
                           ProcessResult &Result) {
  auto Resolved = resolvePath(Path, DirectoryFD, LookupMode::CreateDirectory);
  if (!Resolved)
    return Resolved.takeError();
  if (auto *Error = std::get_if<uint32_t>(&*Resolved))
    return returned(*Error, true);
  if (auto *Reason = std::get_if<const char *>(&*Resolved))
    return unsupported(Result, *Reason);
  const auto &File = std::get<Description>(*Resolved);
  if (File.Type != Kind::Missing)
    return returned(FileExists, true);
  const auto Parent = parentPath(File.Path);
  if (!mutableDirectory(Parent))
    return unsupported(Result, diagnostic::DirectoryNotMutable);
  if (auto E = prepareMutation())
    return std::move(E);
  const uint64_t Charge = File.Path.size() + 1;
  if (File.Path.size() >= limits::Path ||
      FixedEntries + Nodes.size() + Unlinked.size() +
              CreatedDirectories.size() + UnlinkedDirectories.size() >=
          limits::Files ||
      Charge > limits::Bytes - *StorageUsed)
    return unsupported(Result, diagnostic::DirectoryCreationLimit);
  auto Node = std::make_shared<DirectoryNode>();
  Node->Path = File.Path;
  Node->Parent = directoryNode(Parent);
  Node->Identity = directoryIdentity(Parent);
  Node->Created = true;
  CreatedDirectories.emplace(File.Path, std::move(Node));
  *StorageUsed += Charge;
  directoryNode(Parent)->Changed = true;
  return returned(0);
}

std::optional<uint32_t> DarwinFiles::rootRemovalError(const Description &File) {
  if (File.Path != "/")
    return std::nullopt;
  // DELETE lookup rejects a slash-only root before the vnode removal checks.
  // A terminal dot or dotdot resolves the root vnode and reaches VROOT/EBUSY.
  return File.FinalComponent == Terminal::Ordinary ? IsDirectory : ResourceBusy;
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::removeDirectory(uint64_t Path, uint32_t DirectoryFD,
                             ProcessResult &Result) {
  auto Resolved = resolvePath(Path, DirectoryFD, LookupMode::DeleteDirectory);
  if (!Resolved)
    return Resolved.takeError();
  if (auto *Error = std::get_if<uint32_t>(&*Resolved))
    return returned(*Error, true);
  if (auto *Reason = std::get_if<const char *>(&*Resolved))
    return unsupported(Result, *Reason);
  const auto &File = std::get<Description>(*Resolved);
  if (File.Type != Kind::Directory)
    return returned(NotDirectory, true);
  if (auto Error = rootRemovalError(File))
    return returned(*Error, true);
  if (!File.Directory->Created &&
      !Options->RemovableDirectories.contains(File.Path))
    return unsupported(Result, diagnostic::DirectoryRemovalInitial);
  if (File.FinalComponent == Terminal::Dot)
    return returned(InvalidArgument, true);
  if (File.FinalComponent == Terminal::DotDot)
    return returned(File.FinalParentUnlinked ? NoEntry : DirectoryNotEmpty,
                    true);
  const auto Prefix = File.Path + '/';
  const auto ChildFile = Nodes.lower_bound(Prefix);
  const auto ChildDirectory = CreatedDirectories.lower_bound(Prefix);
  if ((ChildFile != Nodes.end() &&
       llvm::StringRef(ChildFile->first).starts_with(Prefix)) ||
      (ChildDirectory != CreatedDirectories.end() &&
       llvm::StringRef(ChildDirectory->first).starts_with(Prefix)) ||
      hasInitialDirectoryChild(File.Path))
    return returned(DirectoryNotEmpty, true);
  if (!mutableDirectory(File.Directory->Parent->Path))
    return unsupported(Result, diagnostic::DirectoryNotMutable);
  if (auto E = prepareMutation())
    return std::move(E);
  File.Directory->Linked = false;
  File.Directory->Changed = true;
  if (File.Directory->Created) {
    UnlinkedDirectories.push_back(File.Directory);
    CreatedDirectories.erase(File.Path);
  }
  File.Directory->Parent->Changed = true;
  return returned(0);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::unlink(uint64_t Path, uint32_t DirectoryFD,
                    ProcessResult &Result) {
  auto Resolved = resolvePath(Path, DirectoryFD, LookupMode::DeleteFile);
  if (!Resolved)
    return Resolved.takeError();
  if (auto *Error = std::get_if<uint32_t>(&*Resolved))
    return returned(*Error, true);
  if (auto *Reason = std::get_if<const char *>(&*Resolved))
    return unsupported(Result, *Reason);
  auto &File = std::get<Description>(*Resolved);
  if (auto Error = rootRemovalError(File))
    return returned(*Error, true);
  if (File.Type == Kind::Directory)
    return returned(OperationNotPermitted, true);
  const auto Parent = parentPath(File.Path);
  if (!mutableDirectory(Parent))
    return unsupported(Result, diagnostic::DirectoryNotMutable);
  if (auto E = prepareMutation())
    return std::move(E);
  updateNamespaceMetadata(*File.File, true);
  directoryNode(Parent)->Changed = true;
  Unlinked.push_back(File.File);
  Nodes.erase(File.Path);
  return returned(0);
}

void DarwinFiles::updateNamespaceMetadata(Contents &Node, bool Removed) {
  if (Node.Policy && !Node.MetadataInvalidated) {
    if (!Node.CurrentMetadata)
      Node.CurrentMetadata = *Node.InitialMetadata;
    if (Removed)
      Node.CurrentMetadata->LinkCount = 0;
    Node.CurrentMetadata->ChangeTime = Node.Policy->Time;
  } else {
    Node.MetadataInvalidated = true;
  }
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::rename(uint64_t SourcePath, uint32_t SourceDirectory,
                    uint64_t TargetPath, uint32_t TargetDirectory,
                    RenameMode Mode, ProcessResult &Result) {
  auto From = resolvePath(SourcePath, SourceDirectory, LookupMode::DeleteFile);
  if (!From)
    return From.takeError();
  if (auto *Error = std::get_if<uint32_t>(&*From))
    return returned(*Error, true);
  if (auto *Reason = std::get_if<const char *>(&*From))
    return unsupported(Result, *Reason);
  auto &Source = std::get<Description>(*From);
  // Directory sources require WILLBEDIR lookup, including different missing
  // trailing-slash rules. Do not apply a regular-file target lookup to them.
  if (Source.Type != Kind::File)
    return unsupported(Result, diagnostic::RenameKind);
  auto To = resolvePath(TargetPath, TargetDirectory, LookupMode::RenameTarget);
  if (!To)
    return To.takeError();
  if (auto *Error = std::get_if<uint32_t>(&*To))
    return returned(*Error, true);
  if (auto *Reason = std::get_if<const char *>(&*To))
    return unsupported(Result, *Reason);
  auto &Target = std::get<Description>(*To);
  if (Mode == RenameMode::Exclusive && (Target.File || Target.Directory)) {
    // Same-object exclusive rename depends on filesystem case sensitivity.
    // Exact catalogue keys do not supply that missing filesystem property.
    if (Target.File == Source.File)
      return unsupported(Result, diagnostic::RenameCaseSensitivity);
    return returned(FileExists, true);
  }
  if (Mode == RenameMode::Swap) {
    if (Target.Type == Kind::Missing)
      return returned(NoEntry, true);
    // Native swap may exchange a regular file with a directory. Its subtree
    // semantics cannot use the ordinary regular-file EISDIR rule.
    if (Target.Type != Kind::File)
      return unsupported(Result, diagnostic::RenameSwapKind);
  }
  const auto Parent = directoryNode(parentPath(Source.Path));
  const auto TargetParent = directoryNode(parentPath(Target.Path));
  // Separate initial directories do not establish a shared mount, even when
  // stat devices match. Only mkdir descendants inherit their parent's domain.
  if (Parent->initialAncestor() != TargetParent->initialAncestor())
    return unsupported(Result, diagnostic::RenameMount);
  // Even a same-name native rename performs authorization. The namespace
  // grant excludes known restricted flags, aliases and special parent modes.
  if (!mutableDirectory(Parent->Path) || !mutableDirectory(TargetParent->Path))
    return unsupported(Result, diagnostic::DirectoryNotMutable);
  std::optional<int32_t> Device;
  auto SameDevice = [&](int32_t D) {
    if (Device && *Device != D)
      return false;
    Device = D;
    return true;
  };
  for (const auto &Directory : {Parent, TargetParent})
    if (const auto Identity = directoryIdentity(Directory->Path);
        Identity && !SameDevice(Identity->Device))
      return unsupported(Result, diagnostic::RenameMount);
  for (const auto *Metadata :
       {Source.File->metadata(),
        Target.File ? Target.File->metadata() : Target.Metadata})
    if (Metadata && !SameDevice(Metadata->Device))
      return unsupported(Result, diagnostic::RenameMount);
  if (Target.Type == Kind::Directory)
    return returned(IsDirectory, true);
  if (Source.Path == Target.Path)
    return returned(0);
  if (Mode == RenameMode::Swap &&
      !Options->SwapRenameDirectories.contains(Parent->initialAncestor()->Path))
    return unsupported(Result, diagnostic::RenameSwapSupport);
  if (auto E = prepareMutation())
    return std::move(E);
  if (Mode == RenameMode::Swap) {
    // Both objects stay linked. Neither their bytes nor their mapping leases
    // provide replacement credit; only their previous dynamic path charges do.
    const uint64_t Other =
        *StorageUsed - Source.File->PathCharge - Target.File->PathCharge;
    const uint64_t SourceCharge = Target.Path.size() + 1;
    const uint64_t TargetCharge = Source.Path.size() + 1;
    if (Target.Path.size() >= limits::Path ||
        Source.Path.size() >= limits::Path ||
        SourceCharge + TargetCharge > limits::Bytes - Other)
      return unsupported(Result, diagnostic::RenameLimit);
    // Allocate every potentially throwing string before extracting either
    // name. Node insertion and string swaps publish the bounded transaction.
    std::string SourceKey = Target.Path, TargetKey = Source.Path;
    std::string SourceIdentity = Target.Path, TargetIdentity = Source.Path;
    auto SourceNode = Nodes.extract(Source.Path);
    auto TargetNode = Nodes.extract(Target.Path);
    SourceNode.key().swap(SourceKey);
    TargetNode.key().swap(TargetKey);
    Nodes.insert(std::move(SourceNode));
    Nodes.insert(std::move(TargetNode));
    Source.File->Path.swap(SourceIdentity);
    Target.File->Path.swap(TargetIdentity);
    Source.File->PathCharge = SourceCharge;
    Target.File->PathCharge = TargetCharge;
    *StorageUsed = Other + SourceCharge + TargetCharge;
    updateNamespaceMetadata(*Source.File, false);
    updateNamespaceMetadata(*Target.File, false);
    Parent->Changed = TargetParent->Changed = true;
    return returned(0);
  }
  // Only Nodes and this lookup may own an immediately reclaimable target.
  // A mapping retains a separate lease even after all descriptors close.
  const uint64_t Credit = Target.File && Target.File.use_count() == 2 &&
                                  Target.File->Lease.use_count() == 1
                              ? Target.bytes().size() + Target.File->PathCharge
                              : 0;
  const uint64_t Other = *StorageUsed - Source.File->PathCharge - Credit;
  const uint64_t Charge = Target.Path.size() + 1;
  if (Target.Path.size() >= limits::Path || Charge > limits::Bytes - Other)
    return unsupported(Result, diagnostic::RenameLimit);
  std::string NewKey = Target.Path, NewIdentity = Target.Path;
  if (Target.File)
    Unlinked.reserve(Unlinked.size() + 1);
  auto Moved = Nodes.extract(Source.Path);
  if (Target.File) {
    updateNamespaceMetadata(*Target.File, true);
    Unlinked.push_back(Target.File);
    Nodes.erase(Target.Path);
  }
  Moved.key().swap(NewKey);
  Nodes.insert(std::move(Moved));
  updateNamespaceMetadata(*Source.File, false);
  Source.File->Path.swap(NewIdentity);
  // Reclaim below deducts Credit exactly once, after the lookup releases it.
  *StorageUsed = *StorageUsed - Source.File->PathCharge + Charge;
  Source.File->PathCharge = Charge;
  Parent->Changed = TargetParent->Changed = true;
  Target.File.reset();
  reclaimUnlinked();
  return returned(0);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::create(Description &File, uint32_t Mode, ProcessResult &Result) {
  const auto Parent = parentPath(File.Path);
  if (!mutableDirectory(Parent))
    return unsupported(Result, diagnostic::DirectoryNotMutable);
  if (auto E = prepareMutation())
    return std::move(E);
  const uint64_t Charge = File.Path.size() + 1;
  if (File.Path.size() >= limits::Path ||
      FixedEntries + Nodes.size() + Unlinked.size() +
              CreatedDirectories.size() + UnlinkedDirectories.size() >=
          limits::Files ||
      Charge > limits::Bytes - *StorageUsed)
    return unsupported(Result, diagnostic::FileCreationLimit);
  if (Options->CreationPolicy && !NextCreatedInode)
    return unsupported(Result, diagnostic::FileCreationInode);
  auto Node = std::make_shared<Contents>();
  Node->Writable = true;
  Node->Path = File.Path;
  Node->PathCharge = Charge;
  if (Options->CreationPolicy) {
    const auto &Policy = *Options->CreationPolicy;
    // Namespace changes invalidate the parent's complete stat observation,
    // but cannot change these supplied device/group fields.
    const auto ParentIdentity = directoryIdentity(Parent);
    if (!ParentIdentity)
      return unsupported(Result, diagnostic::FileCreationParent);
    auto &M = Node->CurrentMetadata.emplace();
    M.Device = ParentIdentity->Device;
    M.GID = ParentIdentity->GID;
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
  directoryNode(Parent)->Changed = true;
  if (Options->CreationPolicy)
    NextCreatedInode =
        NextCreatedInode == UINT64_MAX ? 0 : NextCreatedInode + 1;
  return returned(0);
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
  if (Service == ServiceKind::Mkdir)
    return makeDirectory(A[0], AtCurrentDirectory, Result);
  if (Service == ServiceKind::MkdirAt)
    return makeDirectory(A[1], A[0], Result);
  if (Service == ServiceKind::Rmdir)
    return removeDirectory(A[0], AtCurrentDirectory, Result);
  if (Service == ServiceKind::Access)
    return access(A[0], AtCurrentDirectory, A[1], Result);
  if (Service == ServiceKind::FaccessAt) {
    const uint32_t Flags = A[3];
    if (Flags & ~uint32_t(AtEffectiveAccess | AtNoFollow | AtNoFollowAny))
      return returned(InvalidArgument, true);
    return access(A[1], A[0], A[2], Result);
  }
  if (Service == ServiceKind::Rename)
    return rename(A[0], AtCurrentDirectory, A[1], AtCurrentDirectory,
                  RenameMode::Replace, Result);
  if (Service == ServiceKind::RenameAt || Service == ServiceKind::RenameAtX) {
    auto Mode = RenameMode::Replace;
    if (Service == ServiceKind::RenameAtX) {
      const uint32_t Flags = A[4];
      if ((Flags & ~(RenameSeclude | RenameSwap | RenameExclusive |
                     RenameNoFollowAny)) ||
          (Flags & (RenameSwap | RenameExclusive)) ==
              (RenameSwap | RenameExclusive))
        return returned(InvalidArgument, true);
      if (Flags & RenameSeclude)
        return unsupported(Result, diagnostic::RenameFlags);
      Mode = Flags & RenameSwap        ? RenameMode::Swap
             : Flags & RenameExclusive ? RenameMode::Exclusive
                                       : RenameMode::Replace;
    }
    return rename(A[1], A[0], A[3], A[2], Mode, Result);
  }
  if (Service == ServiceKind::Unlink)
    return unlink(A[0], AtCurrentDirectory, Result);
  if (Service == ServiceKind::UnlinkAt) {
    const uint32_t Flags = A[2];
    if (Flags & ~uint32_t(AtRemoveDirectory | AtRemoveDatalessDirectory |
                          AtNoFollowAny | AtSystemDiscarded))
      return returned(InvalidArgument, true);
    if (Flags & (AtRemoveDatalessDirectory | AtSystemDiscarded))
      return unsupported(Result, diagnostic::UnlinkFlags);
    if (Flags & AtRemoveDirectory)
      return removeDirectory(A[1], A[0], Result);
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
    CurrentDirectory = Directory.Directory;
    reclaimUnlinked();
    return returned(0);
  }
  const bool Vectored =
      Service == ServiceKind::Readv || Service == ServiceKind::Preadv ||
      Service == ServiceKind::Writev || Service == ServiceKind::Pwritev;
  const bool Writing =
      Service == ServiceKind::Write || Service == ServiceKind::Pwrite ||
      Service == ServiceKind::Writev || Service == ServiceKind::Pwritev;
  const bool Positioned =
      Service == ServiceKind::Pread || Service == ServiceKind::Pwrite ||
      Service == ServiceKind::Preadv || Service == ServiceKind::Pwritev;
  if (Service == ServiceKind::Pwritev && A[3] > INT64_MAX)
    return returned(InvalidArgument, true);
  std::vector<Buffer> Vectors;
  if (Vectored) {
    auto Input = readVectors(A[1], uint32_t(A[2]));
    if (!Input)
      return Input.takeError();
    if (auto *Error = std::get_if<uint32_t>(&*Input))
      return returned(*Error, true);
    if (auto *Reason = std::get_if<const char *>(&*Input))
      return unsupported(Result, *Reason);
    Vectors = std::move(std::get<std::vector<Buffer>>(*Input));
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
  if (Vectored || Writing || Service == ServiceKind::Read ||
      Service == ServiceKind::Pread) {
    const auto Access = File.Flags & OpenAccessMask;
    if ((Writing && !Access) || (!Writing && Access == OpenWriteOnly))
      return returned(BadDescriptor, true);
    const bool Vnode = File.Type == Kind::File || File.Type == Kind::Directory;
    if (Positioned && !Vnode)
      return returned(IllegalSeek, true);
    const Buffer Scalar{A[1], A[2]};
    const llvm::ArrayRef<Buffer> Buffers =
        Vectored ? llvm::ArrayRef<Buffer>(Vectors) : llvm::ArrayRef(Scalar);
    uint64_t Count = 0;
    // Metadata is copied before FD lookup, but uio lengths are admitted only
    // after access and positioned-stream checks. Vnodes have a smaller limit.
    for (const auto &B : Buffers) {
      if (B.Size > uint64_t(INT64_MAX) - Count)
        return returned(InvalidArgument, true);
      Count += B.Size;
    }
    if (Vnode && Count > MaxWriteBytes)
      return returned(InvalidArgument, true);
    const uint64_t Offset = Positioned ? A[3] : File.Offset;
    if (!Writing)
      return read(File, Buffers, Count, Offset, Positioned, Result);
    if (File.Type == Kind::Output || File.Type == Kind::Error)
      return capture(File, Buffers, Count, Vectored, Result);
    return write(File, Buffers, Count, Offset, Positioned, Result);
  }
  switch (Service) {
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
    CurrentDirectory = File.Directory;
    reclaimUnlinked();
    return returned(0);
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
          File.Directory->Changed)
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
      const auto &Path = File.File ? File.File->Path : File.Path;
      if (Path.empty())
        return unsupported(Result, diagnostic::FilePathIdentity);
      return copyout(
          A[2],
          llvm::ArrayRef<uint8_t>(
              reinterpret_cast<const uint8_t *>(Path.c_str()), Path.size() + 1),
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
