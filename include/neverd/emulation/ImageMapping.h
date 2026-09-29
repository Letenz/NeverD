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
enum class ImagePagePadding { Zero, FilePages };
struct ImageMapping {
  uint64_t Address;
  unsigned Permissions;
  std::vector<uint8_t> Bytes;
};

/// A finite plan over loader-owned segments. No parsing, symbol resolution or
/// relocation is performed here. The OS owner supplies bias and privilege and
/// must complete its link policy before publishing executable mappings.
struct ImageMappingPlan {
  uint64_t Entry, Base, MappedBytes;
  std::vector<ImageMapping> Regions;

  /// Reject overlapping page mappings, malformed segment extents and budget
  /// overflow before allocating output bytes. FilePages preserves bytes from
  /// rounded file mappings, then clears BSS and its final page tail. Zero
  /// initializes bytes outside the segment's file extent. Shared-page load
  /// policies require an explicit OS implementation; permissions are not
  /// silently combined.
  static llvm::Expected<ImageMappingPlan>
  create(const BinaryImage &Image, uint64_t Bias, uint64_t PageSize,
         uint64_t MemoryLimit, bool User,
         ImagePagePadding Padding = ImagePagePadding::Zero);
};
} // namespace emulation
} // namespace neverd
#endif
