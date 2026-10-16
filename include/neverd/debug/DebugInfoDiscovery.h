//===- DebugInfoDiscovery.h - Locate and load debug info ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Finds the debug information that belongs to a binary and turns it into a
/// DebugContext.  NeverD supports several debug formats through separate
/// loaders (DWARF, PDB, linker MAP); this is the single place that decides
/// which of them applies to a given image and in what order to try them, so
/// every entry point that opens a binary resolves symbols the same way.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DEBUG_DEBUGINFODISCOVERY_H
#define NEVERD_DEBUG_DEBUGINFODISCOVERY_H

#include "neverd/debug/DebugContext.h"
#include "neverd/debug/MapFile.h"
#include "neverd/debug/SymbolStore.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace neverd {

struct BinaryImage;

/// Which loader produced a DebugContext.
enum class DebugInfoKind { None, DWARF, PDB, Map };

/// Human-readable name of \p Kind, for diagnostics and the C API.
const char *debugInfoKindName(DebugInfoKind Kind);

/// Whether images of \p A can have DWARF, a PDB or a linker MAP at all.  EVM
/// and SBF describe themselves through their own metadata -- an ABI JSON, an
/// Anchor IDL -- so discovery reads nothing for them.
bool searchesDebugInfo(Arch A);

/// Where discovery reads an ELF or Mach-O image's DWARF from.
enum class DWARFLocation {
#define NEVERD_DWARF_LOCATION(Name, Spelling) Name,
#include "neverd/debug/DWARFLocation.def"
};

/// The C API spelling of \p Location.
const char *dwarfLocationName(DWARFLocation Location);

/// A file that holds an image's DWARF.
struct DWARFSource {
  std::filesystem::path Path;
  DWARFLocation Location = DWARFLocation::Input;
};

/// Where to look for debug information beyond the binary itself.
///
/// The names an image's debug information gives rank in one order, however
/// they were found: a PDB or DWARF, which also carry types and lines, before
/// a linker MAP, which names addresses and nothing more.  A MAP therefore
/// fills only the addresses the PDB or DWARF leaves unnamed, and the MAP
/// beside a binary is read only for an image no PDB or DWARF describes.
struct DebugInfoRequest {
  /// Caller-supplied companion files.  A non-empty path is authoritative:
  /// discovery loads exactly that file and reports an error if it yields
  /// nothing, rather than silently falling back to whatever sits next to the
  /// binary.  Someone who names a file wants to know when it is the wrong one.
  std::filesystem::path PDBPath;
  /// The DWARF of an ELF or Mach-O image: a split .debug file, an unstripped
  /// copy of the image, or a .dSYM bundle or the DWARF file inside one.  Its
  /// machine must be the image's, and a build ID or UUID both record must
  /// agree.  It takes the place of the DWARF discovery finds.
  std::filesystem::path DWARFPath;
  /// A named MAP takes the place of the one beside the binary.  It is read
  /// after a PDB or DWARF, named or found, which outrank its names.
  std::filesystem::path MapPath;

  /// Set false to read the image alone.  The escape hatch for a stale or
  /// mismatched companion file that names functions worse than the image does.
  bool Enabled = true;

  /// Set false to leave a PE without the PDB it names unless PDBPath names
  /// one: the user declined it, or chose one that loads after the image.
  /// Other debug information is still found.
  bool SearchPDB = true;
  /// Set false to leave an ELF or Mach-O image without its DWARF, from its
  /// own sections or a companion file: the user declined it.  Other debug
  /// information is still found.
  bool SearchDWARF = true;
  /// Set false to leave the image without the MAP beside it: the user
  /// declined it.  A named MAP still loads.
  bool SearchMap = true;
  /// Set true to read the named MAP even when what it records contradicts
  /// the image -- another build stamp, sections placed elsewhere: the user
  /// confirmed it.  A MAP beside the binary that contradicts it is never
  /// read.
  bool UncheckedMap = false;

  /// The symbol search path, in _NT_SYMBOL_PATH syntax, that replaces the
  /// environment's.  Its directories and stores are searched for the PDB;
  /// its servers are not contacted.
  std::optional<std::string> SymbolPath;

  /// Explicit user choice permits the named PDB to supply analysis input
  /// when the PE's CodeView identity is absent. Automatic discovery never
  /// uses this exception; machine, sections and present identities must match.
  bool AllowMissingPDBIdentity = false;
};

/// A file an image's debug information was read from.
struct DebugInfoFile {
  DebugInfoKind Kind = DebugInfoKind::None;
  std::filesystem::path Path;
  /// The names it published into the image (applyDebugInfo).
  unsigned Applied = 0;
  /// A MAP's dialect, and how it held against the image.
  MapDialect Dialect = MapDialect::MSVC;
  MapCheck Check;
};

/// Outcome of a discovery attempt.
struct DebugInfoResult {
  std::unique_ptr<DebugContext> Context;
  DebugInfoKind Kind = DebugInfoKind::None;

  /// File \c Context was built from; empty when nothing was found.
  std::filesystem::path Path;

  /// A named MAP read beside a PDB or DWARF in \c Context: its names fill
  /// the addresses \c Context leaves unnamed.  Null otherwise.
  std::unique_ptr<DebugContext> Supplement;

  /// The files read, \c Context's first.
  std::vector<DebugInfoFile> Files;

  /// Set only when the caller named a file that failed to load.  An absent
  /// companion file is a normal outcome and leaves this empty.
  std::string Error;

  /// True once a context carrying at least one symbol was loaded.
  explicit operator bool() const { return Context && Context->hasInfo(); }
};

/// The DWARF loadDebugInfo reads for the \p Format image at \p BinaryPath,
/// found without loading it and in the order loadDebugInfo tries, which is
/// GDB's: the image's own sections, a .dSYM bundle beside a Mach-O image, the
/// file a debug directory keeps under an ELF image's build ID, the file its
/// .gnu_debuglink names, then a split .debug file beside it, which is matched
/// by name alone.  A companion that records another build ID than the image,
/// or a linked file of another CRC, is passed over.  Empty for another format
/// or an image without DWARF.
std::optional<DWARFSource> findDWARF(const std::filesystem::path &BinaryPath,
                                     BinaryFormat Format);

/// The MAP loadDebugInfo reads for the image at \p BinaryPath when nothing
/// else describes it: <stem>.map or <name>.map beside it.  Empty when there
/// is none.  Whether it holds against the image is for readMapFile to say.
std::optional<std::filesystem::path>
findMap(const std::filesystem::path &BinaryPath);

/// Why \p Check finds that a MAP describes another build, as a clause:
/// "it records the build stamp 5f5a6b7c, the image 5f5a6b80".
std::string describeMapMismatch(const MapCheck &Check);

/// Locate and load the debug information for \p BinaryPath.
///
/// Native debug info comes first because it is both richer and harder to
/// mismatch than a MAP file: PDB for PE, DWARF (including an adjacent .dSYM or
/// a split .debug companion) for ELF and Mach-O.  A linker MAP is the last
/// resort — it carries names and addresses but no types, source lines, or
/// build identity — and exists for stripped binaries whose build only kept the
/// map.  A MAP the caller names is read beside the native debug information,
/// as Result.Supplement, and one that contradicts the image is refused unless
/// the request accepts it unchecked; a MAP beside the binary that contradicts
/// the image is passed over.
DebugInfoResult loadDebugInfo(const std::filesystem::path &BinaryPath,
                              const BinaryImage &Img,
                              const DebugInfoRequest &Req = {},
                              const LoadProgress &Progress = {});

/// Publish \p Dbg's function symbols into \p Img's symbol table and return how
/// many names it contributed.
///
/// Debug info is a NameOrigin::Stated source, which puts it level with the
/// image's own symbol table, so an address the image already names keeps that
/// name and only placeholders are replaced.  An address the image does not
/// describe at all gains a function symbol.  Publishing into BinaryImage —
/// rather than keeping the names inside the DebugContext — is what lets every
/// consumer (function list, detector, disassembler, emitters) see them without
/// each having to consult the debug context separately.
unsigned applyDebugSymbols(BinaryImage &Img, const DebugContext &Dbg);

/// Publish what \p Found read into \p Img: its context's names, then its
/// supplement's, which so fill only what the context leaves unnamed.  Each
/// file's DebugInfoFile::Applied counts its names; returns their sum.
unsigned applyDebugInfo(BinaryImage &Img, DebugInfoResult &Found);

} // namespace neverd

#endif // NEVERD_DEBUG_DEBUGINFODISCOVERY_H
