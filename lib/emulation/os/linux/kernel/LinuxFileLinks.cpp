//===- LinuxFileLinks.cpp - Explicit guest link target observations
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Implements bounded readlink queries over the workload's immutable catalogue.
//===----------------------------------------------------------------------===//
#include "LinuxFiles.h"
#include "LinuxUserMemory.h"

#include <algorithm>

namespace neverd::emulation::linux_model {
llvm::Expected<std::optional<uint64_t>>
LinuxFiles::readLink(uint32_t Directory, uint64_t PathAddress, uint64_t Address,
                     uint32_t Size, ProcessResult &Result) {
  // Linux readlink/readlinkat take a signed int size, including when called
  // through a libc size_t wrapper. The size check precedes pathname access.
  if (!Size || Size > INT32_MAX)
    return std::optional<uint64_t>(uint64_t(0) - InvalidArgument);
  auto Imported = readPath(PathAddress, true);
  if (!Imported)
    return Imported.takeError();
  if (const auto *Error = std::get_if<uint32_t>(&*Imported))
    return std::optional<uint64_t>(uint64_t(0) - *Error);
  const auto &Name = std::get<std::string>(*Imported);
  if (Name.empty()) {
    if (Directory == AtCurrentDirectory)
      return std::optional<uint64_t>(uint64_t(0) - NoEntry);
    auto I = Descriptors.find(Directory);
    if (I == Descriptors.end())
      return std::optional<uint64_t>(uint64_t(0) - BadDescriptor);
    if (std::holds_alternative<OpenFile>(*I->second))
      return std::optional<uint64_t>(uint64_t(0) - NoEntry);
    return unsupported(Result, FileLinkDescriptor);
  }
  auto Path = parsePath(Name);
  if (!Path)
    return unsupported(Result, FilePathForm);
  // Absolute paths ignore dirfd; targets are copied, never resolved.
  switch (lookupPath(Path->Name, Path->RequiresDirectory)) {
  case PathKind::Missing:
    return std::optional<uint64_t>(uint64_t(0) - NoEntry);
  case PathKind::NotDirectory:
    return std::optional<uint64_t>(uint64_t(0) - NotDirectory);
  case PathKind::File:
  case PathKind::Device:
  case PathKind::Directory:
    return std::optional<uint64_t>(uint64_t(0) - InvalidArgument);
  case PathKind::LinkTraversal:
    return unsupported(Result, FileLinkTraversal);
  case PathKind::SymbolicLink:
    break;
  }
  const auto &Target = Options->SymbolicLinks.at(Path->Name);
  const auto Bytes = llvm::ArrayRef<uint8_t>(Target).take_front(
      std::min<size_t>(Target.size(), Size));
  auto Stored = writeUserMemory(CPU, Layout, Address, Bytes);
  if (!Stored)
    return Stored.takeError();
  switch (*Stored) {
  case UserWriteResult::Stored:
    return std::optional<uint64_t>(Bytes.size());
  case UserWriteResult::BadAddress:
    return std::optional<uint64_t>(uint64_t(0) - BadAddress);
  case UserWriteResult::MixedAccess:
    return unsupported(Result, FileLinkPartialOutput);
  }
  llvm_unreachable("unknown Linux user-copy outcome");
}
} // namespace neverd::emulation::linux_model
