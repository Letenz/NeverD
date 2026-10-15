//===- DarwinOwnership.cpp - Explicit ordinary ownership mutations ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Original bounded model of pinned XNU ownership actions and explicitly
// declared provider effects. See docs/darwin-emulation.md for proof limits.
#include "DarwinFiles.h"

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
} // namespace

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::changeOwnership(ServiceKind Service,
                             const ProcessServiceEvent &Event,
                             ProcessResult &Result) {
  const auto &A = Event.Arguments;
  Description File{Kind::Missing, {}};
  const bool At = Service == ServiceKind::FchownAt;
  OwnershipRequest Requested{uint32_t(A[At ? 2 : 1]), uint32_t(A[At ? 3 : 2])};
  if (Service == ServiceKind::Fchown) {
    auto I = Descriptors.find(uint32_t(A[0]));
    if (I == Descriptors.end())
      return returned(BadDescriptor, true);
    File = *I->second.Open;
  } else {
    const uint32_t Flags = At ? uint32_t(A[4]) : 0;
    // The outer fchownat syscall admits only final-link NOFOLLOW. Its inner
    // walker supports ANY for other callers; that is not an outer flag grant.
    if (Flags & ~uint32_t(AtNoFollow))
      return returned(InvalidArgument, true);
    auto Resolved = resolvePath(
        At ? A[1] : A[0], At ? uint32_t(A[0]) : uint32_t(AtCurrentDirectory),
        LookupMode::Existing,
        {Service != ServiceKind::Lchown && !(Flags & AtNoFollow), false}, true,
        Subject::Effective, LookupAuthorization::Ownership);
    if (!Resolved)
      return Resolved.takeError();
    if (auto *Error = std::get_if<uint32_t>(&*Resolved))
      return returned(*Error, true);
    if (auto *Reason = std::get_if<const char *>(&*Resolved))
      return unsupported(Result, *Reason);
    File = std::get<Description>(std::move(*Resolved));
  }
  if (File.Type != Kind::File && File.Type != Kind::Directory &&
      File.Type != Kind::SymbolicLink)
    return unsupported(Result, diagnostic::FileOwnershipTarget);
  auto Current = statusMetadata(File);
  if (auto *Reason = std::get_if<const char *>(&Current))
    return unsupported(Result, *Reason);
  const auto *Metadata = std::get<const DarwinFileMetadata *>(Current);
  auto Authority = authorize(Metadata, 0, Subject::Effective,
                             AuthorizationPurpose::OwnershipChange, Requested);
  if (auto *Error = std::get_if<uint32_t>(&Authority))
    return returned(*Error, true);
  if (auto *Reason = std::get_if<const char *>(&Authority))
    return unsupported(Result, *Reason);
  const auto *ProviderTime = File.Type == Kind::File ? File.File->OwnershipTime
                             : File.Type == Kind::Directory
                                 ? File.Directory->OwnershipTime
                                 : File.Link->OwnershipTime;
  if (!ProviderTime)
    return unsupported(Result, diagnostic::FileOwnershipProvider);
  if (Requested.UID != UINT32_MAX || Requested.GID != UINT32_MAX) {
    // Complete ordinary directory metadata implies its independently retained
    // creation identity. Check before committing either observation.
    if (File.Type == Kind::Directory && Requested.GID != UINT32_MAX &&
        !File.Directory->Identity)
      return unsupported(Result, diagnostic::FileCreationParent);
    auto &CurrentMetadata = File.Type == Kind::File ? File.File->CurrentMetadata
                            : File.Type == Kind::Directory
                                ? File.Directory->CurrentMetadata
                                : File.Link->CurrentMetadata;
    CurrentMetadata = *Metadata;
    if (Requested.UID != UINT32_MAX)
      CurrentMetadata->UID = Requested.UID;
    if (Requested.GID != UINT32_MAX) {
      CurrentMetadata->GID = Requested.GID;
      // All child creation uses this actual object, including after moves or
      // later namespace invalidation. Names never supply an old group again.
      if (File.Type == Kind::Directory)
        File.Directory->Identity->GID = Requested.GID;
    }
    CurrentMetadata->ChangeTime = *ProviderTime;
  }
  return returned(0);
}
} // namespace neverd::emulation::darwin_model
