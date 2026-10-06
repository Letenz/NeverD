//===- LinuxFileSystemStatus.cpp - Guest filesystem query boundaries -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxFiles.h"

namespace neverd::emulation::linux_model {
llvm::Expected<std::optional<uint64_t>>
LinuxFiles::fileSystemStatus(uint64_t Address, ProcessResult &Result) {
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
    // File contents and stat metadata do not describe a mounted filesystem.
    // Path resolution precedes filesystem observation and user output access.
    return unsupported(Result, FileSystemStatusMissing);
  }
  llvm_unreachable("unknown Linux catalogue path kind");
}

std::optional<uint64_t>
LinuxFiles::fileSystemStatusDescriptor(uint32_t FD, ProcessResult &Result) {
  if (!Descriptors.count(FD))
    return uint64_t(0) - BadDescriptor;
  return unsupported(Result, FileSystemStatusMissing);
}
} // namespace neverd::emulation::linux_model
