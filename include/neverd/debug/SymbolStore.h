//===- SymbolStore.h - Where the PDB a PE names is kept ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The PDB a PE image names in its CodeView record, and where copies of it
/// are kept: where the linker wrote it, beside the image, in the directories
/// and symbol stores of a search path, and on symbol servers.  A store keeps
/// a PDB as <store>/<name>/<key>/<name>, the layout of Microsoft's symbol
/// server, under the build identity the image records.
///
/// The search path is _NT_SYMBOL_PATH's, which Microsoft's debuggers and IDA
/// read.  Without one, NeverD searches a store in the user's cache directory
/// and fills it from Microsoft's public symbol server.  Nothing here contacts
/// a server: a frontend asks the user, downloads to the path a server entry
/// names, and loads the file through the strict PDB loader.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DEBUG_SYMBOLSTORE_H
#define NEVERD_DEBUG_SYMBOLSTORE_H

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace neverd {

struct BinaryImage;

/// A symbol-store lookup derived only from a unique, valid PE CodeView record.
/// These are path components, not a URL or permission to contact a server.
struct PDBSymbolRequest {
  std::string FileName;
  std::string Key;
};

std::optional<PDBSymbolRequest> pdbSymbolRequest(const BinaryImage &Img);

/// A symbol server and the store that keeps what it serves.
struct SymbolServer {
  std::string URL;
  std::filesystem::path Cache;
};

/// A symbol search path.
struct SymbolPath {
  /// Directories that hold PDBs under their own names.
  std::vector<std::filesystem::path> Directories;
  /// Stores in symbol-server layout on this machine or its network, in the
  /// order to search them.
  std::vector<std::filesystem::path> Stores;
  /// Servers, in the order to ask them.
  std::vector<SymbolServer> Servers;
};

/// Read \p Text in _NT_SYMBOL_PATH syntax.  \p DefaultCache keeps what a
/// server without a store of its own serves, and stands for an empty store;
/// "srv*" alone is Microsoft's public server.
SymbolPath parseSymbolPath(llvm::StringRef Text,
                           const std::filesystem::path &DefaultCache);

/// The store in the user's cache directory, or empty when the system names
/// no cache directory.
std::filesystem::path defaultSymbolCache();

/// The search path \p Override names, or without one the path
/// _NT_ALT_SYMBOL_PATH and _NT_SYMBOL_PATH name together; when neither is
/// set, the default cache in front of Microsoft's public server.
SymbolPath symbolPath(const std::optional<std::string> &Override = {});

/// Where a local copy of a PDB is.
enum class PDBLocation : uint8_t {
  /// The path the linker recorded in the image.
  Recorded,
  /// Next to the image.
  Beside,
  /// A directory of the search path.
  Directory,
  /// A symbol store of the search path.
  Store,
};

/// The name of \p Location in the C API.
const char *pdbLocationName(PDBLocation Location);

struct PDBCandidate {
  std::filesystem::path Path;
  PDBLocation Location;
};

/// The files that may hold the PDB \p Img names, in the order to try them,
/// whether they exist or not: the recorded path, beside the image under the
/// recorded name and the image's own, then the directories and stores of
/// \p Path.  Stores are searched only for an identity pdbSymbolRequest
/// accepts.
std::vector<PDBCandidate> pdbCandidates(const std::filesystem::path &BinaryPath,
                                        const BinaryImage &Img,
                                        const SymbolPath &Path);

/// Where \p Store keeps the PDB \p Request names.
std::filesystem::path symbolStorePath(const std::filesystem::path &Store,
                                      const PDBSymbolRequest &Request);

/// The URL \p Server serves the PDB \p Request names at.
std::string symbolServerURL(const SymbolServer &Server,
                            const PDBSymbolRequest &Request);

} // namespace neverd

#endif // NEVERD_DEBUG_SYMBOLSTORE_H
