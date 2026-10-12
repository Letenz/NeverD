//===- DarwinFileSystems.cpp - Declared filesystem observations ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Original LP64 statfs64 model. Snapshots are explicit virtual observations,
// never host filesystem queries or inferred mount identity.
#include "DarwinFiles.h"

#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>

namespace neverd::emulation::darwin_model {
using namespace value;
namespace {
std::optional<ServiceResult> returned(uint64_t Value, bool Error = false) {
  return ServiceResult{Value, Error};
}
std::optional<ServiceResult> unsupported(ProcessResult &Result,
                                         const char *Reason) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = Reason;
  return std::nullopt;
}
bool validName(llvm::StringRef Name, unsigned Capacity) {
  if (Name.size() >= Capacity || Name.contains('\0'))
    return false;
  const auto *Begin = reinterpret_cast<const llvm::UTF8 *>(Name.data());
  return llvm::isLegalUTF8String(&Begin, Begin + Name.size());
}
} // namespace

bool validFileSystemSnapshot(const DarwinFileSystemSnapshot &Snapshot) {
  return validName(Snapshot.TypeName, 16) &&
         validName(Snapshot.MountPoint, 1024) &&
         validName(Snapshot.MountedFrom, 1024);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::fileSystemStatus(ServiceKind Service,
                              const ProcessServiceEvent &Event,
                              ProcessResult &Result) {
  const auto &A = Event.Arguments;
  Description File{Kind::Missing, {}};
  if (Service == ServiceKind::Statfs64) {
    auto Resolved = resolvePath(A[0], AtCurrentDirectory, LookupMode::Existing,
                                {true, false}, true, Subject::Effective,
                                LookupAuthorization::FileSystem);
    if (!Resolved)
      return Resolved.takeError();
    if (auto *Error = std::get_if<uint32_t>(&*Resolved))
      return returned(*Error, true);
    if (auto *Reason = std::get_if<const char *>(&*Resolved))
      return unsupported(Result, *Reason);
    File = std::get<Description>(std::move(*Resolved));
  } else {
    const auto I = Descriptors.find(uint32_t(A[0]));
    if (I == Descriptors.end())
      return returned(BadDescriptor, true);
    File = *I->second.Open;
  }
  if (File.Type != Kind::File && File.Type != Kind::Directory)
    return unsupported(Result, diagnostic::FileSystemKind);
  if (!Options || !Options->FileSystemObservations)
    return unsupported(Result, diagnostic::FileSystemEnvironment);
  const auto *Snapshot =
      File.File ? File.File->FileSystem : File.Directory->FileSystem;
  if (!Snapshot)
    return unsupported(Result, diagnostic::FileSystemProvider);
  std::array<uint8_t, 2168> Bytes{};
  const auto Put32 = [&](unsigned Offset, uint32_t Value) {
    llvm::support::endian::write32le(Bytes.data() + Offset, Value);
  };
  const auto Put64 = [&](unsigned Offset, uint64_t Value) {
    llvm::support::endian::write64le(Bytes.data() + Offset, Value);
  };
  Put32(0, Snapshot->BlockSize);
  Put32(4, uint32_t(Snapshot->IOSize));
  Put64(8, Snapshot->Blocks);
  Put64(16, Snapshot->FreeBlocks);
  Put64(24, Snapshot->AvailableBlocks);
  Put64(32, Snapshot->Files);
  Put64(40, Snapshot->FreeFiles);
  Put32(48, uint32_t(Snapshot->FileSystemID[0]));
  Put32(52, uint32_t(Snapshot->FileSystemID[1]));
  Put32(56, Snapshot->Owner);
  Put32(60, Snapshot->Type);
  Put32(64, Snapshot->Flags);
  Put32(68, Snapshot->Subtype);
  std::copy(Snapshot->TypeName.begin(), Snapshot->TypeName.end(),
            Bytes.begin() + 72);
  std::copy(Snapshot->MountPoint.begin(), Snapshot->MountPoint.end(),
            Bytes.begin() + 88);
  std::copy(Snapshot->MountedFrom.begin(), Snapshot->MountedFrom.end(),
            Bytes.begin() + 1112);
  Put32(2136, Snapshot->ExtendedFlags);
  return copyout(A[1], Bytes, diagnostic::FileSystemPartialOutput, Result);
}
} // namespace neverd::emulation::darwin_model
