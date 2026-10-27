//===- NeverDSymbolDownload.cpp - Download a PE's missing PDB -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Ask each symbol server the engine names for the PDB a PE names, keep the
/// first file the engine accepts in that server's store, and try a broken
/// transfer again.
///
//===----------------------------------------------------------------------===//

#include "NeverDSymbolDownload.h"

#include "neverd/sdk/NeverDCAPISession.h"
#include "neverd/support/FilePath.h"

#include "llvm/Support/JSON.h"

#include <chrono>
#include <optional>
#include <thread>

namespace neverd::cli {
namespace {

#define NEVERD_SYMBOL_LIMIT(Name, Value) constexpr long long Name = Value;
#include "../common/symbols/SymbolDownloadABI.def"

/// The status a server answers with the file, and the first of a failure of
/// its own.
constexpr long HttpOk = 200;
constexpr long HttpServerError = 500;

/// The engine's answer for \p Input: the "pdb" object, or none.
std::optional<llvm::json::Object> reference(const std::string &Input,
                                            const char *SymbolPath) {
  const char *Text = neverd_pdb_reference_json(Input.c_str(), SymbolPath);
  if (!Text)
    return std::nullopt;
  auto Parsed = llvm::json::parse(Text);
  neverd_free_string(Text);
  if (!Parsed) {
    llvm::consumeError(Parsed.takeError());
    return std::nullopt;
  }
  const auto *Root = Parsed->getAsObject();
  const auto *PDB = Root ? Root->getObject("pdb") : nullptr;
  if (!PDB)
    return std::nullopt;
  return *PDB;
}

/// The path of the copy the engine found, or empty.
std::string foundPath(const llvm::json::Object &PDB) {
  const auto *Found = PDB.getObject("found");
  return Found ? Found->getString("path").value_or("").str() : std::string();
}

bool sameFile(const std::filesystem::path &A, const std::filesystem::path &B) {
  std::error_code EC;
  return std::filesystem::equivalent(A, B, EC) && !EC;
}

} // namespace

bool downloadMissingPDB(const std::string &Input, const char *SymbolPath,
                        const std::function<void(const std::string &)> &Report,
                        const TransferProgress &Progress) {
  const auto PDB = reference(Input, SymbolPath);
  if (!PDB || !foundPath(*PDB).empty())
    return true;
  const std::string Name = PDB->getString("file").value_or("").str();
  const auto *Servers = PDB->getArray("servers");
  if (!Servers || Servers->empty()) {
    Report("no symbol server is configured to download " + Name + " from");
    return false;
  }
  for (const llvm::json::Value &Entry : *Servers) {
    const auto *Server = Entry.getAsObject();
    if (!Server)
      continue;
    const std::string URL = Server->getString("url").value_or("").str();
    const auto Destination =
        std::filesystem::u8path(Server->getString("path").value_or("").str());
    if (URL.empty() || Destination.empty())
      continue;
    std::error_code EC;
    std::filesystem::create_directories(Destination.parent_path(), EC);
    if (EC) {
      Report("cannot create the symbol store directory " +
             pathToUTF8(Destination.parent_path()) + ": " + EC.message());
      continue;
    }
    // The file takes its store name only once it is complete.
    auto Partial = Destination;
    Partial += ".download";
    Report("downloading " + Name + " from " + URL);
    TransferResult Result;
    for (long long Try = 0;; ++Try) {
      Result = fetchURL(URL, Partial, Progress);
      // A connection that broke, or a server that failed, may serve the file
      // on the next try; an answer that the file is not there will not change.
      const bool Transient = !Result.Status || Result.Status >= HttpServerError;
      if (Result.Status == HttpOk || !Transient || Try == TransferRetries)
        break;
      std::this_thread::sleep_for(
          std::chrono::milliseconds(RetryPauseMs * (Try + 1)));
    }
    if (Result.Status != HttpOk) {
      std::filesystem::remove(Partial, EC);
      Report(Result.Status
                 ? "the server answered HTTP " + std::to_string(Result.Status) +
                       (Result.Error.empty() ? "" : ": " + Result.Error)
                 : "the download failed: " + Result.Error);
      continue;
    }
    std::filesystem::rename(Partial, Destination, EC);
    if (EC) {
      const std::string SaveError = EC.message();
      std::filesystem::remove(Partial, EC);
      Report("cannot save " + pathToUTF8(Destination) + ": " + SaveError);
      continue;
    }
    // The engine names the copy that matches the PE; a file that is not it
    // leaves the store again.
    const auto Again = reference(Input, SymbolPath);
    if (Again &&
        sameFile(std::filesystem::u8path(foundPath(*Again)), Destination)) {
      Report("downloaded " + pathToUTF8(Destination));
      return true;
    }
    std::filesystem::remove(Destination, EC);
    Report("the downloaded file is not the PDB of this build");
  }
  return false;
}

} // namespace neverd::cli
