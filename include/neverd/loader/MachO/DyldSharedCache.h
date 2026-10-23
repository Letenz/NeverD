//===- DyldSharedCache.h - Owned shared-cache address space ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Immutable cache files, validated VM mappings, image identities and slide
/// recipes. Cache-relative targets are resolved before per-image analysis.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_MACHO_DYLDSHAREDCACHE_H
#define NEVERD_LOADER_MACHO_DYLDSHAREDCACHE_H

#include "neverd/Common.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/support/LoadProgress.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neverd::macho_loader {

struct DyldCacheMapping {
  va_t Address = 0;
  uint64_t Size = 0;
  uint64_t FileOffset = 0;
  uint64_t SlideOffset = 0;
  uint64_t SlideSize = 0;
  uint64_t Flags = 0;
  uint32_t MaxProtection = 0;
  uint32_t InitialProtection = 0;
  size_t FileIndex = 0;
};

struct DyldCacheImage {
  va_t Address = 0;
  std::string Path;
};

struct DyldCacheImageSegment {
  std::string Name;
  va_t Address = 0;
  uint64_t Size = 0;
  uint64_t FileOffset = 0;
  uint64_t FileSize = 0;
  uint32_t MaxProtection = 0;
  uint32_t InitialProtection = 0;
};

struct DyldCacheImageLayout {
  /// Borrowed from the owning cache. Unknown commands remain available to
  /// their semantic reader; parsing this layout does not interpret them.
  llvm::ArrayRef<uint8_t> HeaderBytes;
  std::vector<DyldCacheImageSegment> Segments;
  std::vector<Section> Sections;
};

struct DyldCacheFixup {
  va_t Slot = 0;
  va_t Target = 0;
  uint64_t EncodedValue = 0;
  uint32_t SlideVersion = 0;
  std::optional<MachOAuthenticatedPointer> Authentication;
};

class DyldSharedCache {
public:
  /// Main file followed by its table's subcaches in table order. A failed
  /// parse publishes no mappings. Returned byte views live with this object.
  static llvm::Expected<DyldSharedCache>
  parse(std::vector<std::unique_ptr<llvm::MemoryBuffer>> Files,
        const LoadProgress &Progress = {});
  static llvm::Expected<DyldSharedCache>
  open(const std::filesystem::path &Path, const LoadProgress &Progress = {});

  llvm::ArrayRef<DyldCacheMapping> mappings() const { return Mappings; }
  llvm::ArrayRef<DyldCacheImage> images() const { return Images; }
  va_t baseAddress() const { return Base; }
  const DyldCacheMapping *mappingFor(va_t Address) const;
  std::optional<llvm::ArrayRef<uint8_t>> readBytes(va_t Address,
                                                   uint64_t Size) const;
  llvm::Expected<DyldCacheImageLayout> readImageLayout(size_t ImageIndex) const;
  /// Translate an embedded Mach-O file offset through that image's segment
  /// commands and the cache mappings. It is not an offset from its header,
  /// nor necessarily an offset in the file containing that header.
  llvm::Expected<llvm::ArrayRef<uint8_t>>
  readImageFileBytes(size_t ImageIndex, uint64_t Offset, uint64_t Size) const;
  /// Decode a complete mapping transactionally. Preserve original bytes and
  /// signing recipes; no offline reader can manufacture a process's PAC.
  llvm::Expected<std::vector<DyldCacheFixup>>
  readSlideFixups(size_t MappingIndex, const LoadProgress &Progress = {}) const;

private:
  std::vector<std::unique_ptr<llvm::MemoryBuffer>> Files;
  std::vector<DyldCacheMapping> Mappings;
  std::vector<DyldCacheImage> Images;
  va_t Base = 0;
};

} // namespace neverd::macho_loader

#endif // NEVERD_LOADER_MACHO_DYLDSHAREDCACHE_H
