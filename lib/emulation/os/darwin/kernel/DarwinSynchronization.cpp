//===- DarwinSynchronization.cpp - Explicit regular-vnode completion -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original bounded owner of independent immediate regular-vnode completions.
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
DarwinFiles::synchronize(SynchronizationKind Purpose,
                         const ProcessServiceEvent &Event,
                         ProcessResult &Result) {
  const auto I = Descriptors.find(uint32_t(Event.Arguments[0]));
  if (I == Descriptors.end())
    return std::optional<ServiceResult>(ServiceResult{BadDescriptor, true});
  const auto &File = *I->second.Open;
  struct CompletionProvider {
    const char *Kind;
    const char *Target;
    const char *Environment;
    const char *Grant;
    bool Present;
    bool Granted;
  };
  const auto Provider = [&]() -> CompletionProvider {
    switch (Purpose) {
    case SynchronizationKind::File:
      return {diagnostic::FileSynchronizationKind,
              diagnostic::FileSynchronizationTarget,
              diagnostic::FileSynchronizationEnvironment,
              diagnostic::FileSynchronizationProvider,
              bool(Options && Options->Synchronization),
              bool(File.File && File.File->Synchronization)};
    case SynchronizationKind::Data:
      return {diagnostic::FileDataSynchronizationKind,
              diagnostic::FileDataSynchronizationTarget,
              diagnostic::FileDataSynchronizationEnvironment,
              diagnostic::FileDataSynchronizationProvider,
              bool(Options && Options->DataSynchronization),
              bool(File.File && File.File->DataSynchronization)};
    case SynchronizationKind::Full:
      return {diagnostic::FileFullSynchronizationKind,
              diagnostic::FileFullSynchronizationTarget,
              diagnostic::FileFullSynchronizationEnvironment,
              diagnostic::FileFullSynchronizationProvider,
              bool(Options && Options->FullSynchronization),
              bool(File.File && File.File->FullSynchronization)};
    case SynchronizationKind::Barrier:
      return {diagnostic::FileBarrierSynchronizationKind,
              diagnostic::FileBarrierSynchronizationTarget,
              diagnostic::FileBarrierSynchronizationEnvironment,
              diagnostic::FileBarrierSynchronizationProvider,
              bool(Options && Options->BarrierSynchronization),
              bool(File.File && File.File->BarrierSynchronization)};
    }
    llvm_unreachable("unknown Darwin completion purpose");
  }();
  if (File.Type != Kind::File && File.Type != Kind::Directory &&
      File.Type != Kind::SymbolicLink)
    return unsupported(Result, Provider.Kind);
  // The fcntl subcommands retain the existing bad-FD-before-query-scope order.
  // Ordinary fsync/fdatasync remain closed by handle's earlier service gate.
  if (queryEnvironment())
    return unsupported(Result, authorizationScope());
  if (File.Type != Kind::File)
    return unsupported(Result, Provider.Target);
  if (!Provider.Present)
    return unsupported(Result, Provider.Environment);
  if (!Provider.Granted)
    return unsupported(Result, Provider.Grant);
  // All successful model mutations have committed before this declared
  // immediate completion. This does not repair observations or flush host I/O.
  return std::optional<ServiceResult>(ServiceResult{0, false});
}
} // namespace neverd::emulation::darwin_model
