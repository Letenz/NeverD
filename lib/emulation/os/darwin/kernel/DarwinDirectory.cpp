//===- DarwinDirectory.cpp - Explicit immutable directory enumeration -----===//
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
  const auto Snapshot = Options->DirectoryContents.find(File.Path);
  if (Snapshot == Options->DirectoryContents.end())
    return Unsupported(diagnostic::DirectoryContents);
  const auto &Contents = Snapshot->second;
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
