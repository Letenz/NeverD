//===- DebugFileIdentity.h - Which build a debug file is of ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The identities a separate debug file shares with the image it was split
/// from: the GNU build ID of an ELF image and its companion, the LC_UUID of
/// a Mach-O image and its .dSYM, and the name and CRC an ELF image's
/// .gnu_debuglink records for its companion.  GDB, LLDB and IDA find and
/// accept companion files by these, and so does debug discovery.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DEBUG_DEBUGFILEIDENTITY_H
#define NEVERD_DEBUG_DEBUGFILEIDENTITY_H

#include "neverd/Common.h"

#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/TargetParser/Triple.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neverd {

/// Whether \p TripleArch is an architecture images of \p A are built for.
bool isTripleArchOf(llvm::Triple::ArchType TripleArch, Arch A);

/// The build identity \p Obj records: the descriptor of an ELF NT_GNU_BUILD_ID
/// note or a Mach-O LC_UUID.  Empty when it records none.
std::vector<uint8_t> recordedBuildIdentity(const llvm::object::ObjectFile &Obj);

/// The companion file an ELF image's .gnu_debuglink names, and the CRC-32 of
/// that file's contents it records.
struct DebugLink {
  std::string FileName;
  uint32_t CRC = 0;
};
std::optional<DebugLink> elfDebugLink(const llvm::object::ObjectFile &Obj);

/// \p Identity in lowercase hexadecimal, as debug directories spell it.
std::string buildIdentityHex(llvm::ArrayRef<uint8_t> Identity);

/// An object file read from disk, with the buffer it was read from.  A
/// universal Mach-O is read as its slice for \p Slice, or its first slice
/// without one; null when the file holds no such object.
struct OpenedObject {
  std::unique_ptr<llvm::MemoryBuffer> Buffer;
  std::unique_ptr<llvm::object::ObjectFile> Object;
};
llvm::Expected<OpenedObject>
openObjectFile(const std::filesystem::path &Path,
               std::optional<Arch> Slice = std::nullopt);

} // namespace neverd

#endif // NEVERD_DEBUG_DEBUGFILEIDENTITY_H
