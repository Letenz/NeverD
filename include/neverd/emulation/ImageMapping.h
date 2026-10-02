//===- ImageMapping.h - Validated executable mapping plans ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_IMAGEMAPPING_H
#define NEVERD_EMULATION_IMAGEMAPPING_H

#include "neverd/emulation/GuestMemory.h"

namespace neverd {
struct BinaryImage;
namespace emulation {
enum class ImagePagePadding { Zero, FilePages, FilePagesPreserveTail };
/// Analysis may patch segment bytes. Process startup must instead see the
/// original file image when guest code owns dynamic relocations.
enum class ImageByteSource { LoaderSegments, OriginalFile };
struct ImageMapping {
  uint64_t Address;
  unsigned Permissions;
  std::vector<uint8_t> Bytes;
};

/// A finite plan over loader-owned segments. No parsing, symbol resolution or
/// relocation is performed here. The OS owner supplies bias, privilege and
/// byte authority. A guest startup routine may own its own relocations.
struct ImageMappingPlan {
  uint64_t Entry, Base, MappedBytes;
  std::vector<ImageMapping> Regions;

  /// Reject overlapping page mappings, malformed segment extents and budget
  /// overflow before allocating output bytes. FilePages preserves bytes from
  /// rounded file mappings, then clears BSS and its final page tail.
  /// FilePagesPreserveTail keeps the complete final file page and zeroes only
  /// subsequent VM pages, as required by Darwin's segment loader. Zero
  /// initializes bytes outside the segment's file extent. Shared-page load
  /// policies require an explicit OS implementation; permissions are not
  /// silently combined. OriginalFile ignores analysis-patched segment bytes
  /// and validates every source span against the original file instead.
  static llvm::Expected<ImageMappingPlan>
  create(const BinaryImage &Image, uint64_t Bias, uint64_t PageSize,
         uint64_t MemoryLimit, bool User,
         ImagePagePadding Padding = ImagePagePadding::Zero,
         ImageByteSource Source = ImageByteSource::LoaderSegments);
};
} // namespace emulation
} // namespace neverd
#endif
