//===- DarwinDirectory.cpp - Explicit Darwin directory enumeration --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Original implementation of the ABI referenced in docs/darwin-emulation.md.
#include "DarwinDirectory.h"

#include "DarwinFiles.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation::darwin_model {
using namespace value;

void DarwinFiles::forEachDirectoryChild(
    const DirectoryNode &Node,
    llvm::function_ref<void(const DirectoryEntryIdentity &)> Visit) const {
  auto Children = [&](const auto &Table, uint8_t Type, auto Metadata) {
    for (const auto &[Path, Child] : Table) {
      if (Child->Parent.get() != &Node)
        continue;
      const auto *M = Metadata(*Child);
      Visit({llvm::StringRef(Path).rsplit('/').second, Type,
             M ? std::optional<uint64_t>(M->Inode) : std::nullopt});
    }
  };
  // Inode identity does not change when complete stat becomes unknown. Never
  // expose another stale metadata field through this membership projection.
  Children(Nodes, 8, [](const Contents &File) { return File.metadata(); });
  Children(Links, 10, [](const LinkNode &Link) {
    return Link.CurrentMetadata ? &*Link.CurrentMetadata : Link.Metadata;
  });
  Children(Directories, 4, [](const DirectoryNode &Directory) {
    return Directory.CurrentMetadata ? &*Directory.CurrentMetadata
                                     : Directory.Metadata;
  });
}

std::optional<DarwinDirectoryContents>
DarwinFiles::enumerationContents(const DirectoryNode &Node) const {
  const auto &Policy = *Node.EnumerationPolicy;
  DarwinDirectoryContents Contents;
  Contents.MinimumBufferSize = Policy.MinimumBufferSize;
  // Empty removed directories retain their objects, but expose no linked
  // records. The original parent relation still belongs to other observers.
  if (!Node.Linked)
    return Contents;
  auto Inode = [](const DirectoryNode &Directory) -> uint64_t {
    const auto *M = Directory.CurrentMetadata ? &*Directory.CurrentMetadata
                                              : Directory.Metadata;
    return M ? M->Inode : 0;
  };
  const uint64_t Self = Inode(Node);
  const uint64_t Parent = Inode(Node.Parent ? *Node.Parent : Node);
  if (!Self || !Parent)
    return std::nullopt;
  bool Known = true;
  forEachDirectoryChild(Node, [&](const DirectoryEntryIdentity &Child) {
    if (!Child.Inode || !*Child.Inode) {
      Known = false;
      return;
    }
    Contents.Entries.push_back(
        {Child.Name.str(), *Child.Inode, Child.Type, 0, Policy.SeekOffset});
  });
  if (!Known)
    return std::nullopt;
  // StringRef compares unsigned bytes independently of host locale/encoding.
  llvm::sort(Contents.Entries, [](const auto &A, const auto &B) {
    return llvm::StringRef(A.Name).compare(B.Name) < 0;
  });
  Contents.Entries.insert(
      Contents.Entries.begin(),
      {{".", Self, 4, 1, Policy.SeekOffset, Policy.InitialMinimumBufferSize},
       {"..", Parent, 4, 2, Policy.SeekOffset}});
  for (size_t I = 2; I != Contents.Entries.size(); ++I)
    Contents.Entries[I].NextOffset = I + 1;
  return Contents;
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::directory(Description &File, uint64_t Address, uint64_t Count,
                       uint64_t Position, ProcessResult &Result) {
  auto Returned = [](uint64_t Value, bool Error = false) {
    return std::optional<ServiceResult>({Value, Error});
  };
  auto Unsupported = [&](const char *Reason) {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = Reason;
    return std::optional<ServiceResult>();
  };
  if (File.Type == Kind::File)
    return Returned(InvalidArgument, true);
  if (File.Type != Kind::Directory)
    return Unsupported(diagnostic::FileDirectoryKind);
  std::optional<DarwinDirectoryContents> Current;
  const auto &Node = *File.Directory;
  if (Node.EnumerationPolicy) {
    if (Node.EnumerationVersion == UINT64_MAX)
      return Unsupported(diagnostic::DirectoryEnumerationLimit);
    if (File.Offset && File.DirectoryVersion != Node.EnumerationVersion)
      return Unsupported(diagnostic::DirectoryEnumerationVersion);
    Current = enumerationContents(Node);
    if (!Current)
      return Unsupported(diagnostic::DirectoryEnumerationIdentity);
  } else {
    if (Node.Changed)
      return Unsupported(diagnostic::DirectoryMutated);
    if (!Node.Snapshot)
      return Unsupported(diagnostic::DirectoryContents);
  }
  const auto &Contents = Current ? *Current : *Node.Snapshot;
  const bool Extended = Count >= DirectoryExtendedMinimum;
  const uint64_t Payload = std::min(
      Extended ? Count - DirectoryFlagsSize : Count, DirectoryPayloadLimit);
  if (Payload < Contents.MinimumBufferSize)
    return Returned(InvalidArgument, true);
  const uint64_t InitialOffset = File.Offset;
  size_t Next = 0;
  if (InitialOffset) {
    auto I = llvm::find_if(Contents.Entries, [&](const auto &Entry) {
      return Entry.NextOffset == InitialOffset;
    });
    if (I == Contents.Entries.end())
      return Unsupported(diagnostic::DirectoryPosition);
    Next = std::distance(Contents.Entries.begin(), I) + 1;
  }
  if (Next < Contents.Entries.size() &&
      Payload < Contents.Entries[Next].MinimumBufferSize)
    return Returned(InvalidArgument, true);
  uint64_t FinalOffset = InitialOffset;
  std::vector<uint8_t> Bytes;
  while (Next < Contents.Entries.size()) {
    const auto &Entry = Contents.Entries[Next];
    const auto Size = directoryRecordSize(Entry.Name.size());
    if (Size > Payload - Bytes.size()) {
      if (Bytes.empty())
        return Returned(InvalidArgument, true);
      break;
    }
    const auto Start = Bytes.size();
    Bytes.resize(Start + Size, 0);
    auto *Record = Bytes.data() + Start;
    llvm::support::endian::write64le(Record, Entry.Inode);
    llvm::support::endian::write64le(Record + 8, Entry.SeekOffset);
    llvm::support::endian::write16le(Record + 16, Size);
    llvm::support::endian::write16le(Record + 18, Entry.Name.size());
    Record[20] = Entry.Type;
    std::copy(Entry.Name.begin(), Entry.Name.end(),
              Record + DirectoryNameOffset);
    FinalOffset = Entry.NextOffset;
    ++Next;
  }
  // Each successful copy is observable before the next one. In particular,
  // a bad position pointer does not undo data bytes or the updated cursor.
  // At EOF the data pointer is not accessed, but the other copies still occur.
  if (!Bytes.empty()) {
    auto Stored =
        copyout(Address, Bytes, diagnostic::DirectoryPartialData, Result);
    if (!Stored || !*Stored || (**Stored).Error)
      return Stored;
  }
  File.Offset = FinalOffset;
  if (Current)
    File.DirectoryVersion = Node.EnumerationVersion;
  std::array<uint8_t, 8> OffsetBytes;
  llvm::support::endian::write64le(OffsetBytes.data(), InitialOffset);
  auto Stored = copyout(Position, OffsetBytes,
                        diagnostic::DirectoryPartialPosition, Result);
  if (!Stored || !*Stored || (**Stored).Error)
    return Stored;
  if (Extended) {
    std::array<uint8_t, 4> Flags{};
    llvm::support::endian::write32le(Flags.data(),
                                     Next == Contents.Entries.size());
    // XNU places the flags using the original unsigned count, independently
    // of its payload cap. Preserve that address arithmetic, including wrap.
    Stored = copyout(Address + Count - DirectoryFlagsSize, Flags,
                     diagnostic::DirectoryPartialFlags, Result);
    if (!Stored || !*Stored || (**Stored).Error)
      return Stored;
  }
  return Returned(Bytes.size());
}
} // namespace neverd::emulation::darwin_model
