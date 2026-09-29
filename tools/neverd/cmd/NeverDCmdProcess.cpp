//===- NeverDCmdProcess.cpp - Explicit process workload routing ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../NeverDCLI.h"

#include "neverd/emulation/ProcessCLIStrings.h"
#include "neverd/emulation/ProcessReportFields.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd::cli {
int runEmulateProcess() {
  namespace field = emulation::process_report;
  auto Session = neverd_session_create();
  if (!Session) {
    llvm::WithColor::error() << field::SessionFailed << '\n';
    return process_cli::Error;
  }
  SessionGuard Guard(Session);
  const char *Report = neverd_emulate_process_json(
      Session, ProcessInput.getValue().c_str(),
      ProcessProfile.getValue().c_str(), ProcessOptions.getValue().c_str());
  if (!Report) {
    llvm::WithColor::error() << takeLastError(Session) << '\n';
    return process_cli::Error;
  }
  auto Parsed = llvm::json::parse(Report);
  llvm::outs() << Report << '\n';
  neverd_free_string(Report);
  if (!Parsed) {
    llvm::WithColor::error() << field::InvalidReport << ": "
                             << llvm::toString(Parsed.takeError()) << '\n';
    return process_cli::Error;
  }
  const auto *Root = Parsed->getAsObject();
  if (!Root || !Root->getString(field::Stop)) {
    llvm::WithColor::error() << field::InvalidReport << '\n';
    return process_cli::Error;
  }
  if (Root->getString(field::Stop) != emulation::process_outcome::Exited)
    return process_cli::Incomplete;
  auto Status = Root->getInteger(field::ExitStatus);
  if (!Status) {
    llvm::WithColor::error() << field::InvalidReport << '\n';
    return process_cli::Error;
  }
  return *Status == 0 ? process_cli::Success : process_cli::GuestFailure;
}
} // namespace neverd::cli
