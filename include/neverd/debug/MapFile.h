//===- MapFile.h - A linker MAP and the image it describes -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A linker MAP names functions and data by address and nothing more, so it
/// ranks below an image's PDB or DWARF.  It also carries no build identity
/// of the kind those have, which makes a MAP of another build easy to load
/// by mistake.  Most dialects still say enough to tell: link.exe and
/// lld-link record the image's build stamp, and the others list the output
/// sections with their addresses and sizes.  This file reads what a MAP
/// states and holds it against an image, so that discovery passes over a MAP
/// of another build and a frontend can say why before its names load.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DEBUG_MAPFILE_H
#define NEVERD_DEBUG_MAPFILE_H

#include "neverd/Common.h"
#include "neverd/debug/DebugContext.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBufferRef.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neverd {

struct BinaryImage;

/// The MAP formats NeverD reads (MapDialects.def).
enum class MapDialect {
#define NEVERD_MAP_DIALECT(Name, Spelling, Linker) Name,
#include "neverd/debug/MapDialects.def"
};

/// The C API spelling of \p Dialect.
const char *mapDialectName(MapDialect Dialect);

/// The dialect \p Content is written in.  Text no other dialect claims reads
/// as link.exe's, whose parser then finds nothing in it.
MapDialect detectMapDialect(llvm::StringRef Content);

/// An output section as a MAP places it.
struct MapSection {
  /// The Mach-O segment that holds it ("__TEXT"); empty elsewhere.
  std::string Segment;
  std::string Name;
  /// Image-relative when MapFacts::RelativeSections says so.
  uint64_t Address = 0;
  uint64_t Size = 0;
};

/// What a MAP says about the image it was written for.
struct MapFacts {
  MapDialect Dialect = MapDialect::MSVC;
  /// The build stamp link.exe and lld-link record, the TimeDateStamp of the
  /// image's file header.
  std::optional<uint32_t> Stamp;
  /// The image base its addresses assume ("Preferred load address is").
  std::optional<uint64_t> PreferredBase;
  /// The output sections it places, for the dialects that list them with
  /// their addresses: every one but link.exe's, whose section table states
  /// offsets into numbered segments.
  std::vector<MapSection> Sections;
  /// Whether Sections hold image-relative addresses (lld-link /lldmap).
  bool RelativeSections = false;
};

/// The parts of an image a MAP is read against and checked with: where its
/// sections are and which hold code, and the build stamp of a PE.
struct MapImageLayout {
  BinaryFormat Format = BinaryFormat::Unknown;
  va_t Base = 0;
  std::optional<uint32_t> Stamp;
  struct Region {
    /// The Mach-O segment ("__TEXT"); empty elsewhere.
    std::string Segment;
    std::string Name;
    va_t Address = 0;
    uint64_t Size = 0;
    /// The bytes a PE section holds in the file, its size rounded up to the
    /// file alignment, which GNU ld lists as the section's size; 0 elsewhere.
    uint64_t FileSize = 0;
    bool Executable = false;
    /// Whether a MAP that sizes this section \p Bytes places it as it is.
    bool sizedAs(uint64_t Bytes) const {
      return Bytes == Size || (FileSize != 0 && Bytes == FileSize);
    }
    bool contains(va_t Addr) const {
      return Addr >= Address && Addr - Address < Size;
    }
  };
  /// The sections in header order: a link.exe MAP numbers a PE's sections
  /// from one in this order.  An ELF image without section headers has its
  /// loadable segments here instead.
  std::vector<Region> Sections;

  bool isCode(va_t Addr) const;
  bool isMapped(va_t Addr) const;
};

/// The layout of the image whose file bytes are \p Bytes, read from its
/// headers alone.  \p Slice picks the slice of a universal Mach-O; without
/// it, or with no slice of it, the first slice is read.  An error for bytes
/// that are no PE, ELF or Mach-O image.
llvm::Expected<MapImageLayout>
readMapImageLayout(llvm::MemoryBufferRef Bytes,
                   std::optional<Arch> Slice = std::nullopt);

/// The layout of the loaded \p Img: its headers, read from the file bytes it
/// keeps or else from \p File, for a PE, ELF or Mach-O image, and its
/// segments for a binary file.
MapImageLayout mapImageLayout(const BinaryImage &Img,
                              const std::filesystem::path &File);

/// How a MAP holds against an image (MapMatches.def).
enum class MapMatch {
#define NEVERD_MAP_MATCH(Name, Spelling) Name,
#include "neverd/debug/MapMatches.def"
};

/// The C API spelling of \p Match.
const char *mapMatchName(MapMatch Match);

/// Why a MAP belongs to another build (MapMatches.def).
enum class MapMismatch {
#define NEVERD_MAP_MISMATCH(Name, Spelling) Name,
#include "neverd/debug/MapMatches.def"
};

/// The C API spelling of \p Reason.
const char *mapMismatchName(MapMismatch Reason);

/// What holding a MAP against an image showed.
struct MapCheck {
  MapMatch Match = MapMatch::Unverified;
  MapMismatch Reason = MapMismatch::None;
  /// Stamp: the stamps the MAP and the image record.
  uint32_t MapStamp = 0, ImageStamp = 0;
  /// Section: the first section the two place differently, as the MAP and
  /// the image place it.
  MapSection MapPlaced, ImagePlaced;
  /// The function names the MAP gives, its data names, and how many of the
  /// functions lie outside the image's code.
  size_t Functions = 0, DataNames = 0, Outside = 0;
};

/// A MAP read against an image.
struct MapRead {
  /// The names it gives; null when it gives none.
  std::unique_ptr<DebugContext> Context;
  MapFacts Facts;
  MapCheck Check;
  /// Why the file could not be read; empty otherwise.
  std::string Error;
};

/// Read the MAP at \p Path against \p Layout: its names, at the addresses the
/// image places them, and how it holds against the image.  Automatic names
/// such as IDA's `sub_140001000`, which only restate an address, are left
/// out.
MapRead readMapFile(const std::filesystem::path &Path,
                    const MapImageLayout &Layout);

} // namespace neverd

#endif // NEVERD_DEBUG_MAPFILE_H
