//===- ImageMappingTests.cpp - Loader segment to guest page contracts -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/ImageMapping.h"
#include "neverd/loader/BinaryImageModel.h"

namespace neverd::emulation {
namespace {
#define NEVERD_USER_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "UserExecutionCases.def"
#undef NEVERD_USER_VALUE
#define NEVERD_SESSION_TEST_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "ExecutionSessionCases.def"
#undef NEVERD_SESSION_TEST_VALUE
BinaryImage makeImage(bool BSS) {
  BinaryImage Image;
  Image.Base = Code;
  Image.Entry = Code + ImageOffset;
  Image.Raw.assign(PageSize, FileByte);
  Segment S;
  S.VA = Image.Entry;
  S.FileOff = ImageOffset;
  S.FileSz = ImageFileSize;
  S.Size = ImageFileSize * (BSS ? 2 : 1);
  S.Data.assign(ImageFileSize, PatchedByte);
  S.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Image.Segments.push_back(std::move(S));
  return Image;
}
TEST(ImageMapping, FilePagesPreservePaddingWhileLoaderFixupsOwnSegmentBytes) {
  for (bool BSS : {false, true}) {
    auto Image = makeImage(BSS);
    auto Plan = llvm::cantFail(ImageMappingPlan::create(
        Image, 0, PageSize, Limit, true, ImagePagePadding::FilePages));
    ASSERT_EQ(Plan.Regions.size(), 1u);
    const auto &R = Plan.Regions[0];
    EXPECT_EQ(R.Address, Code);
    EXPECT_EQ(R.Permissions, Read | Execute | UserAccessible);
    EXPECT_EQ(R.Bytes.size(), PageSize);
    for (size_t I = 0; I < R.Bytes.size(); ++I) {
      const uint8_t Expected = I < ImageOffset                   ? FileByte
                               : I < ImageOffset + ImageFileSize ? PatchedByte
                               : BSS                             ? 0
                                                                 : FileByte;
      ASSERT_EQ(R.Bytes[I], Expected) << I;
    }
  }
}
TEST(ImageMapping, ZeroPaddingAndBiasDoNotGuessRelocationSemantics) {
  auto Image = makeImage(true);
  auto Plan = llvm::cantFail(
      ImageMappingPlan::create(Image, PageSize, PageSize, Limit, false));
  ASSERT_EQ(Plan.Regions.size(), 1u);
  EXPECT_EQ(Plan.Entry, Image.Entry + PageSize);
  EXPECT_EQ(Plan.Regions[0].Address, Code + PageSize);
  EXPECT_EQ(Plan.Regions[0].Bytes.front(), 0u);
  EXPECT_EQ(Plan.Regions[0].Bytes[ImageOffset], PatchedByte);
}
TEST(ImageMapping, DarwinTailPreservesFileBytesUntilTheNextPage) {
  auto Image = makeImage(true);
  Image.Raw.assign(PageSize * 3, FileByte);
  Image.Segments[0].Size = PageSize * 2;
  for (auto Padding :
       {ImagePagePadding::FilePages, ImagePagePadding::FilePagesPreserveTail}) {
    auto Plan = llvm::cantFail(
        ImageMappingPlan::create(Image, 0, PageSize, Limit, true, Padding,
                                 ImageByteSource::OriginalFile));
    ASSERT_EQ(Plan.Regions.size(), 1u);
    const auto &Bytes = Plan.Regions[0].Bytes;
    ASSERT_EQ(Bytes.size(), PageSize * 3);
    const uint64_t FileEnd = Padding == ImagePagePadding::FilePages
                                 ? ImageOffset + ImageFileSize
                                 : PageSize;
    for (size_t I = 0; I < Bytes.size(); ++I)
      ASSERT_EQ(Bytes[I], I < FileEnd ? FileByte : 0) << I;
  }
}
TEST(ImageMapping, OriginalFileMappingsNeverImportAnalysisRelocations) {
  for (auto Padding : {ImagePagePadding::Zero, ImagePagePadding::FilePages,
                       ImagePagePadding::FilePagesPreserveTail}) {
    auto Image = makeImage(true);
    // Raw file facts remain sufficient even when the analysis byte view is
    // unavailable. The caller explicitly chooses which authority to map.
    Image.Segments[0].Data.clear();
    auto Plan = llvm::cantFail(
        ImageMappingPlan::create(Image, PageSize, PageSize, Limit, true,
                                 Padding, ImageByteSource::OriginalFile));
    ASSERT_EQ(Plan.Regions.size(), 1u);
    EXPECT_EQ(Plan.Entry, Image.Entry + PageSize);
    const auto &R = Plan.Regions[0];
    for (size_t I = 0; I < R.Bytes.size(); ++I) {
      const uint8_t Expected =
          Padding == ImagePagePadding::FilePagesPreserveTail ? FileByte
          : I < ImageOffset
              ? (Padding == ImagePagePadding::FilePages ? FileByte : 0)
          : I < ImageOffset + ImageFileSize ? FileByte
                                            : 0;
      ASSERT_EQ(R.Bytes[I], Expected) << I;
    }
    Image.Segments[0].FileOff = Image.Raw.size();
    auto Invalid =
        ImageMappingPlan::create(Image, 0, PageSize, Limit, true, Padding,
                                 ImageByteSource::OriginalFile);
    EXPECT_FALSE(bool(Invalid));
    llvm::consumeError(Invalid.takeError());
  }
}
TEST(ImageMapping,
     RejectsOverlappingPermissionsOverflowAndUnboundedAllocation) {
  auto Image = makeImage(false);
  Image.Segments.push_back(Image.Segments.front());
  Image.Segments.back().VA += ImageFileSize;
  Image.Segments.back().Flags = SegmentFlags::Writable;
  auto Overlap = ImageMappingPlan::create(Image, 0, PageSize, Limit, true);
  EXPECT_FALSE(bool(Overlap));
  llvm::consumeError(Overlap.takeError());
  Image.Segments.pop_back();
  auto Budget =
      ImageMappingPlan::create(Image, 0, PageSize, PageSize - 1, true);
  EXPECT_FALSE(bool(Budget));
  llvm::consumeError(Budget.takeError());
  auto Overflow =
      ImageMappingPlan::create(Image, UINT64_MAX, PageSize, Limit, true);
  EXPECT_FALSE(bool(Overflow));
  llvm::consumeError(Overflow.takeError());
  Image.Segments[0].FileOff = Image.Raw.size() + 1;
  auto FileRange = ImageMappingPlan::create(Image, 0, PageSize, Limit, true,
                                            ImagePagePadding::FilePages);
  EXPECT_FALSE(bool(FileRange));
  llvm::consumeError(FileRange.takeError());
}
} // namespace
} // namespace neverd::emulation
