//===- SessionSidecar.cpp - Atomic private Session sidecar output ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Shared atomic write owner for Session metadata adapters.
#include "SessionImpl.h"

#include "neverd/support/AtomicOutput.h"
#include "neverd/support/FilePath.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

using namespace neverd;
using namespace neverd::sdk;

bool neverd::sdk::saveSidecar(Session *S, const std::filesystem::path &Path,
                              llvm::json::Value Values, llvm::StringRef Kind) {
  const auto UTF8Path = pathToUTF8(Path);
  auto Temporary = llvm::sys::fs::TempFile::create(
      UTF8Path + ".tmp-%%%%%%",
      llvm::sys::fs::owner_read | llvm::sys::fs::owner_write);
  if (!Temporary) {
    S->setError("cannot create " + Kind.str() +
                " temporary file: " + llvm::toString(Temporary.takeError()));
    return false;
  }
  std::error_code WriteError;
  {
    // TempFile owns the descriptor. Clear a handled stream error before its
    // destructor so ENOSPC/EFBIG becomes a C ABI error rather than LLVM abort.
    llvm::raw_fd_ostream OS(Temporary->FD, false);
    OS << Values;
    OS.flush();
    WriteError = OS.error();
    OS.clear_error();
  }
  if (WriteError) {
    llvm::consumeError(
        support::atomic_output::discardTemporaryOutput(*Temporary));
    S->setError("cannot write " + Kind.str() + ": " + WriteError.message());
    return false;
  }
  if (auto Error = support::atomic_output::closeAndCommitTemporaryOutput(
          *Temporary, UTF8Path)) {
    S->setError("cannot save " + Kind.str() + ": " +
                llvm::toString(std::move(Error)));
    return false;
  }
  return true;
}
