//===- ProcessDarwinFilesJSON.cpp - Bounded Darwin file inputs ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "ProcessDarwinFilesJSON.h"

#include "../os/darwin/kernel/DarwinFiles.h"
#include "ProcessJSONInteger.h"

#include "neverd/emulation/ProcessReportFields.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd::emulation {
namespace {
namespace field = process_report;
llvm::Error invalid(llvm::StringRef Name) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 field::DarwinFilesOptions + Name);
}
llvm::Expected<DarwinDirectoryContents>
directoryContents(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object || Object->size() != 2)
    return invalid(field::DirectoryContents);
  const auto *Entries = Object->getArray(field::DirectoryEntries);
  const auto *Minimum = Object->get(field::DirectoryMinimumBuffer);
  auto Limit =
      Minimum ? process_json::integer<uint32_t>(*Minimum) : std::nullopt;
  if (!Entries || !Limit ||
      Entries->size() > darwin_file_limits::DirectoryEntries)
    return invalid(field::DirectoryContents);
  DarwinDirectoryContents Out;
  Out.MinimumBufferSize = *Limit;
  for (const auto &Value : *Entries) {
    const auto *Entry = Value.getAsObject();
    if (!Entry || (Entry->size() != 5 && Entry->size() != 6) ||
        (Entry->size() == 6 && !Entry->get(field::DirectoryMinimumBuffer)))
      return invalid(field::DirectoryEntries);
    const auto Name = Entry->getString(field::Name);
    if (!Name)
      return invalid(field::Name);
    DarwinDirectoryEntry E;
    E.Name = Name->str();
    auto Number = [&](llvm::StringRef Key, auto &Destination) -> llvm::Error {
      const auto *V = Entry->get(Key);
      using T = std::remove_reference_t<decltype(Destination)>;
      auto N = V ? process_json::integer<T>(*V) : std::nullopt;
      if (!N)
        return invalid(Key);
      Destination = *N;
      return llvm::Error::success();
    };
    if (auto Error = Number(field::FileInode, E.Inode))
      return Error;
    if (auto Error = Number(field::DirectoryType, E.Type))
      return Error;
    if (auto Error = Number(field::DirectoryNextOffset, E.NextOffset))
      return Error;
    if (auto Error = Number(field::DirectorySeekOffset, E.SeekOffset))
      return Error;
    if (Entry->get(field::DirectoryMinimumBuffer))
      if (auto Error =
              Number(field::DirectoryMinimumBuffer, E.MinimumBufferSize))
        return Error;
    Out.Entries.push_back(std::move(E));
  }
  return Out;
}
llvm::Expected<DarwinFileTime> fileTime(const llvm::json::Value *Value,
                                        llvm::StringRef Name) {
  const auto *Time = Value ? Value->getAsObject() : nullptr;
  if (!Time || Time->size() != 2 || !Time->get(field::Seconds) ||
      !Time->get(field::Nanoseconds))
    return invalid(Name);
  auto Seconds = process_json::integer<int64_t>(*Time->get(field::Seconds));
  auto Nanoseconds =
      process_json::integer<int64_t>(*Time->get(field::Nanoseconds));
  if (!Seconds || !Nanoseconds)
    return invalid(Name);
  return DarwinFileTime{*Seconds, *Nanoseconds};
}
llvm::Expected<DarwinFileMetadata> metadata(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object || Object->size() != 15)
    return invalid(field::FileMetadata);
  DarwinFileMetadata Out;
  auto Number = [&](llvm::StringRef Name, auto &Destination) -> llvm::Error {
    const auto *V = Object->get(Name);
    using T = std::remove_reference_t<decltype(Destination)>;
    auto N = V ? process_json::integer<T>(*V) : std::nullopt;
    if (!N)
      return invalid(Name);
    Destination = *N;
    return llvm::Error::success();
  };
  if (auto E = Number(field::FileDevice, Out.Device))
    return std::move(E);
  if (auto E = Number(field::FileInode, Out.Inode))
    return std::move(E);
  if (auto E = Number(field::FileMode, Out.Mode))
    return std::move(E);
  if (auto E = Number(field::FileLinkCount, Out.LinkCount))
    return std::move(E);
  if (auto E = Number(field::FileUID, Out.UID))
    return std::move(E);
  if (auto E = Number(field::FileGID, Out.GID))
    return std::move(E);
  if (auto E = Number(field::Size, Out.Size))
    return std::move(E);
  if (auto E = Number(field::FileBlockSize, Out.BlockSize))
    return std::move(E);
  if (auto E = Number(field::FileBlocks, Out.Blocks))
    return std::move(E);
  if (auto E = Number(field::FileFlags, Out.Flags))
    return std::move(E);
  if (auto E = Number(field::FileGeneration, Out.Generation))
    return std::move(E);
  for (const auto &[Name, Destination] :
       {std::pair<llvm::StringRef, DarwinFileTime *>{field::FileAccessTime,
                                                     &Out.AccessTime},
        {field::FileModificationTime, &Out.ModificationTime},
        {field::FileChangeTime, &Out.ChangeTime},
        {field::FileBirthTime, &Out.BirthTime}}) {
    auto Parsed = fileTime(Object->get(Name), Name);
    if (!Parsed)
      return Parsed.takeError();
    *Destination = *Parsed;
  }
  return Out;
}
llvm::Expected<DarwinFileMutationPolicy>
mutationPolicy(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object || Object->size() != 2)
    return invalid(field::FileMutationPolicy);
  const auto *Unit = Object->get(field::FileAllocationUnit);
  auto Number = Unit ? process_json::integer<uint32_t>(*Unit) : std::nullopt;
  if (!Number)
    return invalid(field::FileAllocationUnit);
  auto Time =
      fileTime(Object->get(field::FileMutationTime), field::FileMutationTime);
  if (!Time)
    return Time.takeError();
  return DarwinFileMutationPolicy{*Number, *Time};
}
llvm::Expected<std::vector<uint8_t>> bytes(const llvm::json::Value &Value,
                                           uint64_t &Remaining) {
  auto Hex = Value.getAsString();
  if (!Hex || Hex->size() % 2 || Hex->size() / 2 > Remaining ||
      !llvm::all_of(*Hex, llvm::isHexDigit))
    return invalid(field::Bytes);
  Remaining -= Hex->size() / 2;
  const auto Data = llvm::fromHex(*Hex);
  return std::vector<uint8_t>(Data.begin(), Data.end());
}
} // namespace

llvm::Expected<DarwinFileOptions>
darwinFileOptionsFromJSON(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object || !Object->get(field::Files))
    return invalid(field::DarwinFiles);
  DarwinFileOptions Out;
  uint64_t Remaining = darwin_file_limits::Bytes;
  for (const auto &[Key, V] : *Object) {
    const llvm::StringRef Name = Key;
    if (Name == field::DescriptorLimit) {
      auto N = V.getAsUINT64();
      if (!N || *N > UINT32_MAX)
        return invalid(Name);
      Out.DescriptorLimit = *N;
    } else if (Name == field::StandardInput) {
      auto Data = bytes(V, Remaining);
      if (!Data)
        return Data.takeError();
      Out.StandardInput = std::move(*Data);
    } else if (Name == field::WorkingDirectory) {
      auto Path = V.getAsString();
      if (!Path || Path->size() >= Remaining)
        return invalid(Name);
      Remaining -= Path->size() + 1;
      Out.WorkingDirectory = Path->str();
    } else if (Name == field::Directories) {
      const auto *Directories = V.getAsArray();
      if (!Directories || Directories->size() > darwin_file_limits::Files)
        return invalid(Name);
      for (const auto &Directory : *Directories) {
        const auto *D = Directory.getAsObject();
        if (!D ||
            D->size() != 1 + unsigned(bool(D->get(field::FileMetadata))) +
                             unsigned(bool(D->get(field::DirectoryContents))))
          return invalid(Name);
        auto Path = D->getString(field::Path);
        if (!Path || Path->size() >= Remaining ||
            !Out.Directories.emplace(Path->str()).second)
          return invalid(field::Path);
        Remaining -= Path->size() + 1;
        if (const auto *C = D->get(field::DirectoryContents)) {
          auto Parsed = directoryContents(*C);
          if (!Parsed)
            return Parsed.takeError();
          Out.DirectoryContents.emplace(Path->str(), std::move(*Parsed));
        }
        if (const auto *M = D->get(field::FileMetadata)) {
          auto Parsed = metadata(*M);
          if (!Parsed)
            return Parsed.takeError();
          Out.Metadata.emplace(Path->str(), std::move(*Parsed));
        }
      }
    } else if (Name == field::Files) {
      const auto *Files = V.getAsArray();
      if (!Files || Files->size() > darwin_file_limits::Files)
        return invalid(Name);
      for (const auto &File : *Files) {
        const auto *F = File.getAsObject();
        if (!F || !F->get(field::Bytes) ||
            F->size() != 2 + unsigned(bool(F->get(field::FileMetadata))) +
                             unsigned(bool(F->get(field::FileWritable))) +
                             unsigned(bool(F->get(field::FileMutationPolicy))))
          return invalid(Name);
        auto Path = F->getString(field::Path);
        if (!Path || Path->size() >= Remaining)
          return invalid(field::Path);
        Remaining -= Path->size() + 1;
        auto Data = bytes(*F->get(field::Bytes), Remaining);
        if (!Data)
          return Data.takeError();
        if (!Out.Files.emplace(Path->str(), std::move(*Data)).second)
          return invalid(field::Path);
        if (const auto *W = F->get(field::FileWritable)) {
          auto Writable = W->getAsBoolean();
          if (!Writable)
            return invalid(field::FileWritable);
          if (*Writable)
            Out.WritableFiles.insert(Path->str());
        }
        if (const auto *M = F->get(field::FileMetadata)) {
          auto Parsed = metadata(*M);
          if (!Parsed)
            return Parsed.takeError();
          Out.Metadata.emplace(Path->str(), std::move(*Parsed));
        }
        if (const auto *P = F->get(field::FileMutationPolicy)) {
          auto Parsed = mutationPolicy(*P);
          if (!Parsed)
            return Parsed.takeError();
          Out.MutationPolicies.emplace(Path->str(), *Parsed);
        }
      }
    } else {
      return invalid(Name);
    }
  }
  if (auto E = darwin_model::validateFileOptions(Out))
    return std::move(E);
  return Out;
}
} // namespace neverd::emulation
