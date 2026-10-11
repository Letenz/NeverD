//===- DarwinModes.cpp - Explicit ordinary regular mode mutations --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Original bounded model of XNU chmod_vnode/vnode_authattr; source and native
// proof limits are described in docs/darwin-emulation.md.
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
DarwinFiles::changeMode(ServiceKind Service, const ProcessServiceEvent &Event,
                        ProcessResult &Result) {
  const auto &A = Event.Arguments;
  Description File{Kind::Missing, {}};
  uint32_t Requested = uint32_t(A[1]) & FilePermissionMask;
  if (Service == ServiceKind::Fchmod) {
    auto I = Descriptors.find(uint32_t(A[0]));
    if (I == Descriptors.end())
      return returned(BadDescriptor, true);
    File = *I->second.Open;
  } else {
    const bool At = Service == ServiceKind::FchmodAt;
    const uint32_t Flags = At ? uint32_t(A[3]) : 0;
    if (Flags & ~uint32_t(AtNoFollow | AtNoFollowAny))
      return returned(InvalidArgument, true);
    if (At)
      Requested = uint32_t(A[2]) & FilePermissionMask;
    auto Resolved = resolvePath(
        At ? A[1] : A[0], At ? uint32_t(A[0]) : uint32_t(AtCurrentDirectory),
        LookupMode::Existing,
        {!(Flags & (AtNoFollow | AtNoFollowAny)), bool(Flags & AtNoFollowAny)},
        true, Subject::Effective, LookupAuthorization::Mode);
    if (!Resolved)
      return Resolved.takeError();
    if (auto *Error = std::get_if<uint32_t>(&*Resolved))
      return returned(*Error, true);
    if (auto *Reason = std::get_if<const char *>(&*Resolved))
      return unsupported(Result, *Reason);
    File = std::get<Description>(std::move(*Resolved));
  }
  if (File.Type != Kind::File)
    return unsupported(Result, diagnostic::FileModeTarget);
  auto Current = statusMetadata(File);
  if (auto *Reason = std::get_if<const char *>(&Current))
    return unsupported(Result, *Reason);
  const auto *Metadata = std::get<const DarwinFileMetadata *>(Current);
  auto Authority = authorize(Metadata, Requested, Subject::Effective,
                             AuthorizationPurpose::ModeChange);
  if (auto *Error = std::get_if<uint32_t>(&Authority))
    return returned(*Error, true);
  if (auto *Reason = std::get_if<const char *>(&Authority))
    return unsupported(Result, *Reason);
  auto &Node = *File.File;
  if (!Node.ModeTime)
    return unsupported(Result, diagnostic::FileModeProvider);
  Node.CurrentMetadata = *Metadata;
  Node.CurrentMetadata->Mode =
      (Metadata->Mode & ~FilePermissionMask) | Requested;
  Node.CurrentMetadata->ChangeTime = *Node.ModeTime;
  return returned(0);
}
} // namespace neverd::emulation::darwin_model
