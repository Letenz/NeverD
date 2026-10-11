//===- DarwinSynchronization.cpp - Explicit regular-vnode completion -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original bounded model of an explicitly declared immediate fsync provider.
// See docs/darwin-emulation.md for provider, cancellation and durability
// limits.
#include "DarwinFiles.h"

namespace neverd::emulation::darwin_model {
using namespace value;
namespace {
std::optional<ServiceResult> unsupported(ProcessResult &Result,
                                         const char *Reason) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = Reason;
  return std::nullopt;
}
} // namespace

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::synchronize(const ProcessServiceEvent &Event,
                         ProcessResult &Result) {
  const auto I = Descriptors.find(uint32_t(Event.Arguments[0]));
  if (I == Descriptors.end())
    return std::optional<ServiceResult>(ServiceResult{BadDescriptor, true});
  const auto &File = *I->second.Open;
  if (File.Type != Kind::File && File.Type != Kind::Directory &&
      File.Type != Kind::SymbolicLink)
    return unsupported(Result, diagnostic::FileSynchronizationKind);
  if (File.Type != Kind::File)
    return unsupported(Result, diagnostic::FileSynchronizationTarget);
  if (!Options || !Options->Synchronization)
    return unsupported(Result, diagnostic::FileSynchronizationEnvironment);
  if (!File.File || !File.File->Synchronization)
    return unsupported(Result, diagnostic::FileSynchronizationProvider);
  // All successful model mutations have committed before this declared
  // immediate completion. This does not repair observations or flush host I/O.
  return std::optional<ServiceResult>(ServiceResult{0, false});
}
} // namespace neverd::emulation::darwin_model
