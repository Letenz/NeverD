//===- DarwinTimestamps.cpp - Declared ordinary timestamp mutations
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Original bounded utimes/futimes model; provider rules and proof limits are
// described in docs/darwin-emulation.md. No host clock/filesystem is consulted.
#include "DarwinFiles.h"
#include "DarwinTime.h"
#include "DarwinUserMemory.h"

#include "llvm/ADT/APInt.h"
#include "llvm/Support/Endian.h"

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
DarwinFileTime storedTime(const uint8_t *Bytes) {
  const auto Seconds = llvm::APInt(64, llvm::support::endian::read64le(Bytes));
  const auto Microseconds =
      llvm::APInt(32, llvm::support::endian::read32le(Bytes + 8));
  auto Wide = Seconds.sext(128) * 1000000000 + Microseconds.sext(128) * 1000;
  const auto Minimum = llvm::APInt::getSignedMinValue(64).sext(128);
  const auto Maximum = llvm::APInt::getSignedMaxValue(64).sext(128);
  Wide = Wide.slt(Minimum) ? Minimum : Wide.sgt(Maximum) ? Maximum : Wide;
  const auto N = Wide.getSExtValue();
  DarwinFileTime Time{N / 1000000000, N % 1000000000};
  if (Time.Nanoseconds < 0) {
    --Time.Seconds;
    Time.Nanoseconds += 1000000000;
  }
  return Time;
}
bool earlier(const DarwinFileTime &A, const DarwinFileTime &B) {
  return A.Seconds < B.Seconds ||
         (A.Seconds == B.Seconds && A.Nanoseconds < B.Nanoseconds);
}
} // namespace

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::changeTimes(ServiceKind Service, const ProcessServiceEvent &Event,
                         ProcessResult &Result,
                         const std::optional<DarwinTimeOptions> &Time) {
  const auto &A = Event.Arguments;
  Description File{Kind::Missing, {}};
  // utimes resolves the pathname before importing the array. futimes imports
  // first, including the explicit NULL sample, before narrowing/looking up FD.
  if (Service == ServiceKind::Utimes) {
    auto Resolved = resolvePath(A[0], AtCurrentDirectory, LookupMode::Existing,
                                {true, false}, true, Subject::Effective,
                                LookupAuthorization::Timestamp);
    if (!Resolved)
      return Resolved.takeError();
    if (auto *Error = std::get_if<uint32_t>(&*Resolved))
      return returned(*Error, true);
    if (auto *Reason = std::get_if<const char *>(&*Resolved))
      return unsupported(Result, *Reason);
    File = std::get<Description>(std::move(*Resolved));
  }
  DarwinFileTime Access, Modification;
  if (A[1]) {
    std::array<uint8_t, 32> Bytes;
    auto Prefix = userMemoryPrefix(Memory, A[1], Bytes.size(), Read);
    if (!Prefix)
      return Prefix.takeError();
    if (!*Prefix)
      return returned(BadAddress, true);
    if (*Prefix != Bytes.size())
      return unsupported(Result, diagnostic::FileTimestampInput);
    if (auto E = Memory.read(A[1], Bytes))
      return std::move(E);
    // Padding is deliberately ignored; signed microseconds are normalized,
    // never guessed invalid. Both records are immutable before any effects.
    Access = storedTime(Bytes.data());
    Modification = storedTime(Bytes.data() + 16);
  } else {
    if (!Time || !Time->TimeOfDay)
      return unsupported(Result, diagnostic::TimeOfDayMissing);
    if (auto E = validateTimeOptions(*Time))
      return std::move(E);
    Access = Modification = {Time->TimeOfDay->Seconds,
                             int64_t(Time->TimeOfDay->Microseconds) * 1000};
  }
  if (Service == ServiceKind::Futimes) {
    auto I = Descriptors.find(uint32_t(A[0]));
    if (I == Descriptors.end())
      return returned(BadDescriptor, true);
    File = *I->second.Open;
  }
  if (File.Type == Kind::SymbolicLink)
    return unsupported(Result, diagnostic::FileTimestampTarget);
  if (File.Type != Kind::File && File.Type != Kind::Directory)
    return unsupported(Result, diagnostic::FileTimestampKind);
  auto Current = statusMetadata(File);
  if (auto *Reason = std::get_if<const char *>(&Current))
    return unsupported(Result, *Reason);
  const auto *Metadata = std::get<const DarwinFileMetadata *>(Current);
  auto Authority =
      authorize(Metadata, 2, Subject::Effective,
                A[1] ? AuthorizationPurpose::TimestampChangeExplicit
                     : AuthorizationPurpose::TimestampChangeCurrent);
  if (auto *Error = std::get_if<uint32_t>(&Authority))
    return returned(*Error, true);
  if (auto *Reason = std::get_if<const char *>(&Authority))
    return unsupported(Result, *Reason);
  const auto *ProviderTime = File.Type == Kind::File
                                 ? File.File->TimestampTime
                                 : File.Directory->TimestampTime;
  if (!ProviderTime)
    return unsupported(Result, diagnostic::FileTimestampProvider);
  auto &CurrentMetadata = File.Type == Kind::File
                              ? File.File->CurrentMetadata
                              : File.Directory->CurrentMetadata;
  CurrentMetadata = *Metadata;
  CurrentMetadata->AccessTime = Access;
  CurrentMetadata->ModificationTime = Modification;
  CurrentMetadata->ChangeTime = *ProviderTime;
  if (Modification.Seconds >= 0 && earlier(Modification, Metadata->BirthTime))
    CurrentMetadata->BirthTime = Modification;
  return returned(0);
}
} // namespace neverd::emulation::darwin_model
