//===- CompressedSections.h - Expand compressed ELF sections ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Distribution debug packages and `-gz` builds store an ELF file's DWARF
/// sections compressed (SHF_COMPRESSED, zlib or zstd).  The LLVM NeverD links
/// may lack the decompressors its DWARF reader needs, so the DWARF loader
/// hands it a copy of the file whose compressed sections are expanded.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DEBUG_COMPRESSEDSECTIONS_H
#define NEVERD_DEBUG_COMPRESSEDSECTIONS_H

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace neverd {

/// The ELF file \p Bytes with every compressed section expanded: each one's
/// bytes are appended to the copy and its section header points at them,
/// uncompressed, so a reader finds the file otherwise as it was.  Nothing
/// when no section is compressed, when \p Bytes are no ELF file, or when a
/// section cannot be expanded -- an unknown or unavailable compression, a
/// stream that does not hold the size its header states, or more than
/// limits::kMaxExpandedDebugSections in all.
std::optional<std::vector<uint8_t>>
expandCompressedSections(llvm::StringRef Bytes);

} // namespace neverd

#endif // NEVERD_DEBUG_COMPRESSEDSECTIONS_H
