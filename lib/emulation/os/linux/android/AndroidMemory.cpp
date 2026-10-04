//===- AndroidMemory.cpp - Bounded copies of explicit memory inputs ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/FileSystem.h"

#include <algorithm>
#include <array>

namespace neverd::emulation::android_model {
namespace {
llvm::Error ioFailure(llvm::Error E) {
  return failure(llvm::Twine(diagnostic::MemoryInputIO) +
                 llvm::toString(std::move(E)));
}
llvm::Error ioFailure(std::error_code E) {
  return ioFailure(llvm::errorCodeToError(E));
}
} // namespace

llvm::Error initializeMemoryRegion(AddressSpace &Space,
                                   const NativeMemoryRegion &Region,
                                   const ExecutionBudget &Budget) {
  if (!Region.File)
    return Region.Bytes.empty() ? llvm::Error::success()
                                : Space.write(Region.Address, Region.Bytes);
  if (!Region.Bytes.empty())
    return failure(diagnostic::MemoryInputConflict);
  const auto &NativePath = Region.File->native();
  if (NativePath.empty() ||
      NativePath.find(std::filesystem::path::value_type{}) != NativePath.npos)
    return failure(diagnostic::MemoryInputPath);
  auto Expired = [&] { return !Budget.remainingMicroseconds(); };
  if (Expired())
    return failure(diagnostic::MemoryInputDeadline);
  const auto UTF8 = Region.File->u8string();
  llvm::StringRef Path(reinterpret_cast<const char *>(UTF8.data()),
                       UTF8.size());
  namespace fs = llvm::sys::fs;
  fs::file_status Before;
  if (auto E = fs::status(Path, Before))
    return ioFailure(E);
  // Reject directories and streams before opening; recheck the opened object.
  if (!fs::is_regular_file(Before))
    return failure(diagnostic::MemoryInputRegular);
  if (Before.getSize() > Region.Size)
    return failure(diagnostic::MemoryInputExtent);
  if (Expired())
    return failure(diagnostic::MemoryInputDeadline);
  auto File = fs::openNativeFileForRead(Path);
  if (!File)
    return ioFailure(File.takeError());
  auto Close = llvm::make_scope_exit([&] { fs::closeFile(*File); });
  fs::file_status Opened;
  if (auto E = fs::status(*File, Opened))
    return ioFailure(E);
  if (!fs::is_regular_file(Opened))
    return failure(diagnostic::MemoryInputRegular);
  if (Opened.getSize() != Before.getSize() ||
      Opened.getUniqueID() != Before.getUniqueID())
    return failure(diagnostic::MemoryInputChanged);

  std::array<char, 64 * 1024> Buffer;
  uint64_t Offset = 0, Size = Opened.getSize();
  while (Offset < Size) {
    if (Expired())
      return failure(diagnostic::MemoryInputDeadline);
    auto Count = fs::readNativeFile(
        *File, llvm::MutableArrayRef(Buffer).take_front(
                   std::min<uint64_t>(Size - Offset, Buffer.size())));
    if (!Count)
      return ioFailure(Count.takeError());
    if (!*Count)
      return failure(diagnostic::MemoryInputChanged);
    if (auto E = Space.write(
            Region.Address + Offset,
            {reinterpret_cast<const uint8_t *>(Buffer.data()), *Count}))
      return E;
    Offset += *Count;
  }
  if (Expired())
    return failure(diagnostic::MemoryInputDeadline);
  auto Extra =
      fs::readNativeFile(*File, llvm::MutableArrayRef(Buffer).take_front(1));
  if (!Extra)
    return ioFailure(Extra.takeError());
  fs::file_status After;
  if (auto E = fs::status(*File, After))
    return ioFailure(E);
  if (*Extra || After.getSize() != Size)
    return failure(diagnostic::MemoryInputChanged);
  if (Expired())
    return failure(diagnostic::MemoryInputDeadline);
  return llvm::Error::success();
}
} // namespace neverd::emulation::android_model
