//===- NeverDSymbolDownload.h - Download a PE's missing PDB ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The command line's --download-symbols: the PDB a PE names, when no copy on
/// this computer matches it, comes from the symbol servers the engine names
/// (neverd_pdb_reference_json) into the store the engine searches, so the
/// load that follows finds it.  The engine judges the file; this transport
/// only fetches bytes, through libcurl or WinHTTP, the system's own.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_SYMBOLDOWNLOAD_H
#define NEVERD_TOOLS_NEVERD_SYMBOLDOWNLOAD_H

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace neverd::cli {

/// Bytes received and expected so far; Total is 0 while unknown.
using TransferProgress = std::function<void(uint64_t Received, uint64_t Total)>;

/// How one HTTP transfer ended.
struct TransferResult {
  /// The server's status, or 0 when no answer arrived.
  long Status = 0;
  /// Why no answer arrived, or why the body could not be kept.
  std::string Error;
};

/// GET \p URL into \p File, following redirects and within the transfer
/// limits of common/symbols/SymbolDownloadABI.def.  One transport is built per
/// platform.
TransferResult fetchURL(const std::string &URL,
                        const std::filesystem::path &File,
                        const TransferProgress &Progress);

/// Download the PDB the PE at \p Input names when no local copy matches it,
/// as \p SymbolPath (in _NT_SYMBOL_PATH syntax, or null for the
/// environment's) says where; \p Report gets each line to show.  Returns
/// false when the PE names a PDB that no server provided; a PE that names
/// none, or a copy already here, is no failure.
bool downloadMissingPDB(const std::string &Input, const char *SymbolPath,
                        const std::function<void(const std::string &)> &Report,
                        const TransferProgress &Progress);

} // namespace neverd::cli

#endif // NEVERD_TOOLS_NEVERD_SYMBOLDOWNLOAD_H
