//===- RichHeader.h - PE Rich header @comp.id records ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Microsoft's linker writes, between a PE file's DOS stub and its PE header,
/// which build of which tool produced the objects it linked: the "Rich
/// header".  Every linked object counts, including the ones in the static
/// runtime libraries, and the linker records itself.  The descriptions of the
/// records come from richprint's comp_id.txt; see
/// lib/loader/COFF/RichCompIds.inc and LICENSES/richprint.txt.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_COFF_RICHHEADER_H
#define NEVERD_LOADER_COFF_RICHHEADER_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace neverd {

/// One record: a product (tool), a build of it, and how many objects that
/// build of that tool contributed.
struct RichEntry {
  uint16_t ProdId = 0;
  uint16_t Build = 0;
  uint32_t Count = 0;
};

/// A decoded Rich header.
struct RichHeader {
  /// File offset of the "DanS" marker, where the block begins.
  uint32_t Offset = 0;
  /// The XOR key stored after the "Rich" marker.  The linker computes it as a
  /// checksum over the DOS header and the records.
  uint32_t Key = 0;
  /// Whether \ref Key is the checksum of what the file holds.  A block edited
  /// after linking no longer is, and says nothing reliable about the tools.
  bool ChecksumMatches = false;
  std::vector<RichEntry> Entries;
};

/// Decode the Rich header of a PE file image.
///
/// Returns std::nullopt when the file has none, which is the case for images
/// that a non-Microsoft linker produced, and also when the block that is
/// there does not decode: no "DanS" marker, padding that is not zero, or
/// records that do not fit before the PE header.
std::optional<RichHeader> decodeRichHeader(llvm::ArrayRef<uint8_t> File);

/// The kind of tool a product id is, from richprint's marks.
enum class RichTool : uint8_t {
  Unknown,
  Unmarked,
  C,
  Cpp,
  Asm,
  Linker,
  Resource,
  Import,
  Export,
  LtcgC,
  LtcgCpp,
  LtcgMsil,
  PogoInstrumentC,
  PogoInstrumentCpp,
  PogoOptimizeC,
  PogoOptimizeCpp,
  CvtcilC,
  CvtcilCpp,
  Cvtomf,
  Cvtpgd,
  AliasObj,
  Basic,
  IlAsm,
};

/// What richprint's table says about one record.
struct RichToolInfo {
  RichTool Tool = RichTool::Unknown;
  /// The Visual Studio release the build belongs to, such as 2026, or 0 when
  /// the table cannot tell.
  unsigned VisualStudioYear = 0;
  /// richprint's description of the exact build, or of the product when the
  /// build is not listed.
  llvm::StringRef Description;
  /// Whether the table lists this exact build.
  bool ExactBuild = false;
};

/// Describe a record.  A build the table does not list takes the release of
/// the nearest earlier build of the same product: from Visual Studio 2015 on,
/// every release shares its product ids and only the build numbers, which
/// only grow, tell the releases apart.
RichToolInfo describeRichEntry(uint16_t ProdId, uint16_t Build);

/// The Visual Studio releases whose linker produced the image, taken from
/// its linker records.  Without a linker record, the releases of the tools
/// that produced its code.  Empty when the header's checksum does not match,
/// or when no record names a release.
std::vector<unsigned> richToolsetYears(const RichHeader &Header);

} // namespace neverd

#endif // NEVERD_LOADER_COFF_RICHHEADER_H
