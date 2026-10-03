//===- ImageMapping.cpp - Prepare finite mappings from loader segments ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/emulation/ImageMapping.h"

#include "neverd/loader/BinaryImageModel.h"

#include <algorithm>
#include <limits>

namespace neverd::emulation {
namespace {
#define NEVERD_IMAGE_DIAGNOSTIC(Name, Text) constexpr char Name[] = Text;
#include "ImageMappingDiagnostics.def"
#undef NEVERD_IMAGE_DIAGNOSTIC
llvm::Error failure(const char *Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
} // namespace

llvm::Expected<ImageMappingPlan>
ImageMappingPlan::create(const BinaryImage &Image, uint64_t Bias,
                         uint64_t PageSize, uint64_t MemoryLimit, bool User,
                         ImagePagePadding Padding, ImageByteSource Source) {
  if (!PageSize || (PageSize & (PageSize - 1)) || !MemoryLimit)
    return failure(Configuration);
  if (Padding != ImagePagePadding::Zero &&
      Padding != ImagePagePadding::FilePages &&
      Padding != ImagePagePadding::FilePagesPreserveTail)
    return failure(Configuration);
  if (Source != ImageByteSource::LoaderSegments &&
      Source != ImageByteSource::OriginalFile)
    return failure(Configuration);
  const auto Max = std::numeric_limits<uint64_t>::max();
  if (Image.Entry > Max - Bias || Image.Base > Max - Bias)
    return failure(Extent);
  struct Region {
    uint64_t Address, Size;
    const Segment *Source;
    uint64_t FileOffset, FileBytes;
  };
  std::vector<Region> Regions;
  uint64_t Mapped = 0;
  for (const auto &Segment : Image.Segments) {
    if (!Segment.Size) {
      if (Segment.FileSz ||
          (Source == ImageByteSource::LoaderSegments && !Segment.Data.empty()))
        return failure(Extent);
      continue;
    }
    if (Segment.FileSz > Segment.Size || Segment.VA > Max - Bias ||
        Segment.Size > Max - (Segment.VA + Bias))
      return failure(Extent);
    if (Source == ImageByteSource::LoaderSegments &&
        (Segment.Data.size() < Segment.FileSz ||
         Segment.Data.size() > Segment.Size))
      return failure(Extent);
    if (Source == ImageByteSource::OriginalFile &&
        (Segment.FileOff > Image.Raw.size() ||
         Segment.FileSz > Image.Raw.size() - Segment.FileOff))
      return failure(Extent);
    const uint64_t VA = Segment.VA + Bias;
    const uint64_t End = VA + Segment.Size;
    if (End > Max - (PageSize - 1))
      return failure(Extent);
    const uint64_t Base = VA & ~(PageSize - 1);
    const uint64_t Size = ((End + PageSize - 1) & ~(PageSize - 1)) - Base;
    if (Size > MemoryLimit - Mapped ||
        Size > std::numeric_limits<size_t>::max())
      return failure(Budget);
    uint64_t FileOffset = 0, FileBytes = 0;
    if (Padding != ImagePagePadding::Zero) {
      const uint64_t Prefix = VA - Base;
      if (Segment.FileOff < Prefix || Segment.FileOff > Image.Raw.size() ||
          Segment.FileSz > Image.Raw.size() - Segment.FileOff)
        return failure(Extent);
      FileOffset = Segment.FileOff - Prefix;
      const uint64_t FileEnd = Prefix + Segment.FileSz;
      FileBytes = std::min((FileEnd + PageSize - 1) & ~(PageSize - 1),
                           uint64_t(Image.Raw.size()) - FileOffset);
    }
    Mapped += Size;
    Regions.push_back({Base, Size, &Segment, FileOffset, FileBytes});
  }
  if (Regions.empty())
    return failure(Empty);
  llvm::sort(Regions, [](const auto &A, const auto &B) {
    return A.Address < B.Address;
  });
  for (size_t I = 1; I < Regions.size(); ++I)
    if (Regions[I - 1].Size > Regions[I].Address - Regions[I - 1].Address)
      return failure(Overlap);

  ImageMappingPlan Plan{Image.Entry + Bias, Image.Base + Bias, Mapped, {}};
  Plan.Regions.reserve(Regions.size());
  for (const auto &Region : Regions) {
    const auto &Segment = *Region.Source;
    const unsigned Permissions = (Segment.isReadable() ? Read : 0u) |
                                 (Segment.isWritable() ? Write : 0u) |
                                 (Segment.isExecutable() ? Execute : 0u) |
                                 (User ? UserAccessible : 0u);
    ImageMapping Mapping{Region.Address, Permissions,
                         std::vector<uint8_t>(Region.Size, 0)};
    if (Region.FileBytes)
      std::copy_n(Image.Raw.begin() + Region.FileOffset, Region.FileBytes,
                  Mapping.Bytes.begin());
    if (Padding != ImagePagePadding::FilePagesPreserveTail &&
        Segment.Size > Segment.FileSz)
      std::fill(Mapping.Bytes.begin() + (Segment.VA + Bias - Region.Address) +
                    Segment.FileSz,
                Mapping.Bytes.end(), 0);
    const auto Bytes = Source == ImageByteSource::OriginalFile
                           ? Image.Raw.begin() + Segment.FileOff
                           : Segment.Data.begin();
    std::copy_n(Bytes, Segment.FileSz,
                Mapping.Bytes.begin() + (Segment.VA + Bias - Region.Address));
    Plan.Regions.push_back(std::move(Mapping));
  }
  return Plan;
}
} // namespace neverd::emulation
