//===- DarwinAdvisoryLocks.cpp - Explicit Darwin whole-file locks --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original bounded description-owned model of pinned XNU flock actions.
// See docs/darwin-emulation.md for declared provider and wait proof limits.
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
DarwinFiles::advisoryLock(const ProcessServiceEvent &Event,
                          ProcessResult &Result) {
  const auto &A = Event.Arguments;
  auto I = Descriptors.find(uint32_t(A[0]));
  if (I == Descriptors.end())
    return returned(BadDescriptor, true);
  auto &File = *I->second.Open;
  if (File.Type != Kind::File && File.Type != Kind::Directory &&
      File.Type != Kind::SymbolicLink)
    return unsupported(Result, diagnostic::FileAdvisoryLockKind);
  const uint32_t How = A[1];
  AdvisoryLock Requested;
  if (How & LockUnlock)
    Requested = AdvisoryLock::None;
  else if (How & LockExclusive)
    Requested = AdvisoryLock::Exclusive;
  else if (How & LockShared)
    Requested = AdvisoryLock::Shared;
  else
    return returned(BadDescriptor, true);
  if (File.Type != Kind::File)
    return unsupported(Result, diagnostic::FileAdvisoryLockTarget);
  if (!Options || !Options->AdvisoryLocks)
    return unsupported(Result, diagnostic::FileAdvisoryLockEnvironment);
  if (!File.File || !File.File->AdvisoryLocks)
    return unsupported(Result, diagnostic::FileAdvisoryLockProvider);
  if (Requested == AdvisoryLock::None) {
    File.Lock = Requested;
    return returned(0);
  }
  std::set<const Description *> Seen;
  for (const auto &[Number, Descriptor] : Descriptors) {
    const auto &Other = *Descriptor.Open;
    if (&Other == &File || Other.Type != Kind::File ||
        Other.File != File.File || Other.Lock == AdvisoryLock::None ||
        !Seen.insert(&Other).second)
      continue;
    if (Requested == AdvisoryLock::Exclusive ||
        Other.Lock == AdvisoryLock::Exclusive) {
      // A nonblocking conflict precedes old-lock removal in lf_setlock.
      // The asynchronous blocking path is refused before any mutation.
      if (How & LockNonblocking)
        return returned(LockWouldBlock, true);
      return unsupported(Result, diagnostic::FileAdvisoryLockWait);
    }
  }
  File.Lock = Requested;
  // Pinned XNU sets FWASLOCKED only after a successful lock acquisition.
  // Unlock preserves this description history; failures never create it.
  File.Flags |= FileWasLocked;
  return returned(0);
}
} // namespace neverd::emulation::darwin_model
