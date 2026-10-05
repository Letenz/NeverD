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
    const auto *Time = Object->getObject(Name);
    if (!Time || Time->size() != 2 || !Time->get(field::Seconds) ||
        !Time->get(field::Nanoseconds))
      return invalid(Name);
    auto Seconds = process_json::integer<int64_t>(*Time->get(field::Seconds));
    auto Nanoseconds =
        process_json::integer<int64_t>(*Time->get(field::Nanoseconds));
    if (!Seconds || !Nanoseconds)
      return invalid(Name);
    *Destination = {*Seconds, *Nanoseconds};
  }
  return Out;
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
        if (!D || (D->size() != 1 && D->size() != 2) ||
            (D->size() == 2 && !D->get(field::FileMetadata)))
          return invalid(Name);
        auto Path = D->getString(field::Path);
        if (!Path || Path->size() >= Remaining ||
            !Out.Directories.emplace(Path->str()).second)
          return invalid(field::Path);
        Remaining -= Path->size() + 1;
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
        if (!F || (F->size() != 2 && F->size() != 3) || !F->get(field::Bytes) ||
            (F->size() == 3 && !F->get(field::FileMetadata)))
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
        if (const auto *M = F->get(field::FileMetadata)) {
          auto Parsed = metadata(*M);
          if (!Parsed)
            return Parsed.takeError();
          Out.Metadata.emplace(Path->str(), std::move(*Parsed));
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
