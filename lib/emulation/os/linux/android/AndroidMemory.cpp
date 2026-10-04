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
namespace input {
#define NEVERD_ANDROID_MEMORY_TEXT(Name, Text) constexpr char Name[] = Text;
#define NEVERD_ANDROID_MEMORY_VALUE(Name, Value) constexpr size_t Name = Value;
#include "AndroidMemoryInputs.def"
#undef NEVERD_ANDROID_MEMORY_VALUE
#undef NEVERD_ANDROID_MEMORY_TEXT
} // namespace input
llvm::Error ioFailure(llvm::Error E) {
  return failure(llvm::Twine(input::FileIO) + llvm::toString(std::move(E)));
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
    return failure(input::ConflictingInputs);
  const auto &NativePath = Region.File->native();
  if (NativePath.empty() ||
      NativePath.find(std::filesystem::path::value_type{}) != NativePath.npos)
    return failure(input::InvalidPath);
  auto Expired = [&] { return !Budget.remainingMicroseconds(); };
  if (Expired())
    return failure(input::Deadline);
  const auto UTF8 = Region.File->u8string();
  llvm::StringRef Path(reinterpret_cast<const char *>(UTF8.data()),
                       UTF8.size());
  namespace fs = llvm::sys::fs;
  fs::file_status Before;
  if (auto E = fs::status(Path, Before))
    return ioFailure(E);
  // Reject directories and streams before opening; recheck the opened object.
  if (!fs::is_regular_file(Before))
    return failure(input::RegularFile);
  if (Before.getSize() > Region.Size)
    return failure(input::FileExtent);
  if (Expired())
    return failure(input::Deadline);
  auto File = fs::openNativeFileForRead(Path);
  if (!File)
    return ioFailure(File.takeError());
  auto Close = llvm::make_scope_exit([&] { fs::closeFile(*File); });
  fs::file_status Opened;
  if (auto E = fs::status(*File, Opened))
    return ioFailure(E);
  if (!fs::is_regular_file(Opened))
    return failure(input::RegularFile);
  if (Opened.getSize() != Before.getSize() ||
      Opened.getUniqueID() != Before.getUniqueID())
    return failure(input::FileChanged);

  std::array<char, input::ReadChunk> Buffer;
  uint64_t Offset = 0, Size = Opened.getSize();
  while (Offset < Size) {
    if (Expired())
      return failure(input::Deadline);
    auto Count = fs::readNativeFile(
        *File, llvm::MutableArrayRef(Buffer).take_front(
                   std::min<uint64_t>(Size - Offset, Buffer.size())));
    if (!Count)
      return ioFailure(Count.takeError());
    if (!*Count)
      return failure(input::FileChanged);
    if (auto E = Space.write(
            Region.Address + Offset,
            {reinterpret_cast<const uint8_t *>(Buffer.data()), *Count}))
      return E;
    Offset += *Count;
  }
  if (Expired())
    return failure(input::Deadline);
  auto Extra =
      fs::readNativeFile(*File, llvm::MutableArrayRef(Buffer).take_front(1));
  if (!Extra)
    return ioFailure(Extra.takeError());
  fs::file_status After;
  if (auto E = fs::status(*File, After))
    return ioFailure(E);
  if (*Extra || After.getSize() != Size)
    return failure(input::FileChanged);
  if (Expired())
    return failure(input::Deadline);
  return llvm::Error::success();
}
} // namespace neverd::emulation::android_model
