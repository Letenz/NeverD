//===- LoadCandidate.h - The ways NeverD can load a file --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// What the load dialog lists for a file before it is opened: every loader
// that accepts the file, as which format and processor, and whether loading
// it that way can succeed.  Each loader answers from the same header checks
// and processor mapping its load() uses.
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_LOADCANDIDATE_H
#define NEVERD_LOADER_LOADCANDIDATE_H

#include "neverd/Common.h"
#include "neverd/loader/Raw/ISAIdentify.h"

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace neverd {

/// A row of the load dialog (LoadRows.def).
enum class LoadRow : uint8_t {
#define NEVERD_LOAD_ROW(Id, Loader, Text) Id,
#include "neverd/loader/LoadRows.def"
};

/// Why a row cannot be loaded (LoadRows.def).
enum class LoadReason : uint8_t {
#define NEVERD_LOAD_REASON(Id, Text) Id,
#include "neverd/loader/LoadRows.def"
};

/// The short name of the loader that reads \p Row, such as "elf".
llvm::StringRef getLoadRowLoader(LoadRow Row);
/// The llvm::formatv pattern of \p Row's text.
llvm::StringRef getLoadRowText(LoadRow Row);
/// The llvm::formatv pattern of \p Reason's text.
llvm::StringRef getLoadReasonText(LoadReason Reason);

/// One way a loader can read a file.
struct LoadCandidate {
  LoadRow Row = LoadRow::Binary;
  /// The row text, such as "ELF64 for x86-64 (Shared object)".
  std::string Description;
  BinaryFormat Format = BinaryFormat::Unknown;
  /// The processor the header states, Unknown when NeverD has none for it.
  Arch TheArch = Arch::Unknown;
  /// The word size the header states, 0 when it states none.
  unsigned Bits = 0;
  bool BigEndian = false;
  /// Whether the header and processor allow loading the file this way; the
  /// load can still fail on a malformed body.
  bool Loadable = false;
  /// Why the row cannot be loaded, empty when it can.
  std::string Reason;
  /// Listed before the other rows, as a specific format beats a generic one.
  bool First = false;
  /// The loader took the file for its name alone, not its contents, so a
  /// load dialog does not choose the row by default.
  bool ByName = false;
  /// The binary file row of a file no header describes: the instruction set
  /// its bytes show.
  std::optional<ISAIdentification> ISA;
};

/// Every loader's rows for the file at \p Path in list order: the rows that
/// lead, then the others in loader order, then "Binary file", which accepts
/// any file.  The file is read no further than a loader needs to tell its
/// format.
std::vector<LoadCandidate> identifyFile(const std::filesystem::path &Path);

} // namespace neverd

#endif // NEVERD_LOADER_LOADCANDIDATE_H
