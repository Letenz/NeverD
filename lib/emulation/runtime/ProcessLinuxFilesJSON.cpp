//===- ProcessLinuxFilesJSON.cpp - Bounded explicit file inputs ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "ProcessLinuxFilesJSON.h"

#include "../os/linux/kernel/LinuxFiles.h"
#include "ProcessJSONInteger.h"

#include "neverd/emulation/ProcessReportFields.h"

#include "llvm/ADT/StringExtras.h"

#include <utility>

namespace neverd::emulation {
namespace {
namespace field = process_report;
llvm::Error invalid(llvm::StringRef Name) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 field::LinuxFilesOptions + Name);
}

llvm::Expected<LinuxFileMetadata> metadata(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object || Object->size() != 12)
    return invalid(field::FileMetadata);
  LinuxFileMetadata Out;
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
  for (const auto &[Name, Destination] :
       {std::pair<llvm::StringRef, LinuxTimespec *>{field::FileAccessTime,
                                                    &Out.AccessTime},
        {field::FileModificationTime, &Out.ModificationTime},
        {field::FileChangeTime, &Out.ChangeTime}}) {
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
} // namespace

llvm::Expected<LinuxFileOptions>
linuxFileOptionsFromJSON(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object || !Object->get(field::Files))
    return invalid(field::LinuxFiles);
  LinuxFileOptions Out;
  for (const auto &[Key, V] : *Object) {
    llvm::StringRef Name = Key;
    if (Name == field::DescriptorLimit) {
      auto N = V.getAsUINT64();
      if (!N || *N > UINT32_MAX)
        return invalid(Name);
      Out.DescriptorLimit = *N;
      continue;
    }
    if (Name != field::Files)
      return invalid(Name);
    const auto *Files = V.getAsArray();
    if (!Files || Files->size() > linux_model::FileCountLimit)
      return invalid(Name);
    for (const auto &File : *Files) {
      const auto *F = File.getAsObject();
      if (!F || (F->size() != 2 && F->size() != 3) ||
          (F->size() == 3 && !F->get(field::FileMetadata)))
        return invalid(Name);
      auto Path = F->getString(field::Path);
      auto Bytes = F->getString(field::Bytes);
      if (!Path || !Bytes || Bytes->size() % 2 ||
          !llvm::all_of(*Bytes, llvm::isHexDigit))
        return invalid(Name);
      std::string Data = llvm::fromHex(*Bytes);
      if (!Out.Files
               .emplace(Path->str(),
                        std::vector<uint8_t>(Data.begin(), Data.end()))
               .second)
        return invalid(field::Path);
      if (const auto *M = F->get(field::FileMetadata)) {
        auto Parsed = metadata(*M);
        if (!Parsed)
          return Parsed.takeError();
        Out.Metadata.emplace(Path->str(), std::move(*Parsed));
      }
    }
  }
  if (auto E = linux_model::validateFileOptions(Out))
    return std::move(E);
  return Out;
}
} // namespace neverd::emulation
