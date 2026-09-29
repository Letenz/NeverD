//===- ProcessSession.cpp - Explicit process-profile dispatch ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/emulation/ProcessSession.h"

#include "../core/ExecutionDiagnostics.h"
#include "../os/linux/LinuxProcess.h"
#include "RuntimeValues.h"

#include "llvm/Support/ErrorHandling.h"

namespace neverd::emulation {
llvm::Expected<ProcessProfile> parseProcessProfile(llvm::StringRef Name) {
#define NEVERD_PROCESS_PROFILE(ID, Text)                                       \
  if (Name == Text)                                                            \
    return ProcessProfile::ID;
#include "neverd/emulation/ProcessProfile.def"
#undef NEVERD_PROCESS_PROFILE
  return diagnostic::error(runtime::ProcessProfile);
}
const char *processProfileName(ProcessProfile Profile) {
  switch (Profile) {
#define NEVERD_PROCESS_PROFILE(ID, Text)                                       \
  case ProcessProfile::ID:                                                     \
    return Text;
#include "neverd/emulation/ProcessProfile.def"
#undef NEVERD_PROCESS_PROFILE
  }
  llvm_unreachable(runtime::ProcessProfile);
}
const char *processStopReasonName(ProcessStopReason Reason) {
  switch (Reason) {
#define NEVERD_PROCESS_STOP(ID, Text)                                          \
  case ProcessStopReason::ID:                                                  \
    return Text;
#include "neverd/emulation/ProcessProfile.def"
#undef NEVERD_PROCESS_STOP
  }
  llvm_unreachable(runtime::ProcessOutcome);
}
llvm::Expected<ProcessResult> emulateProcess(const std::filesystem::path &Path,
                                             ProcessProfile Profile,
                                             const ProcessOptions &Options) {
  switch (Profile) {
  case ProcessProfile::LinuxELF64:
    return linux_model::runProcess(Path, Options);
  }
  return diagnostic::error(runtime::ProcessProfile);
}
} // namespace neverd::emulation
