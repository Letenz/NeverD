//===- COFFExceptionV2Tests.cpp - Normalized x64 V2 unwind records --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFExceptionTestsDetail.h"

#include "neverd/loader/COFF/COFFException.h"

#include <algorithm>
#include <vector>

using namespace neverd;
using namespace neverd::coff_eh_test;

namespace {
TEST(COFFExceptionParser,
     NormalizesV2EpiloguesSeparatelyFromRestoreOperations) {
  BinaryImage Img = makeX64ExceptionImage();
  const std::vector<uint8_t> Bytes = {
      2,    6,    6, 0, // V2, prologue size, slot count, no frame register.
      4,    0x16,  // Four-byte epilogues, including the one at function end.
      0x40, 0x16,  // Distance 0x140 from function end.
      0xe0, 0x16,  // Distance 0x1e0: deliberately unordered.
      0,    6,     // Epilogue padding, not a prologue operation.
      6,    0x32,  // Allocate 32 bytes.
      1,    0x30}; // Push rbx.
  std::copy(Bytes.begin(), Bytes.end(), Img.Segments[1].Data.begin());
  const auto F = coff_loader::decodeX64ExceptionFunction(
      Img, Img.Base, 0x2000, 0x1000, 0x1200, 0x3000);
  EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Complete);
  EXPECT_EQ(F.Encoding, ExceptionEncoding::X64UnwindV2);
  ASSERT_EQ(F.Epilogs.size(), 3u);
  EXPECT_EQ(F.Epilogs[0].StartOffset, 0x20);
  EXPECT_EQ(F.Epilogs[1].StartOffset, 0xc0);
  EXPECT_EQ(F.Epilogs[2].StartOffset, 0x1fc);
  for (const auto &E : F.Epilogs) {
    EXPECT_EQ(E.LastInstructionOffset, 3u);
    EXPECT_TRUE(E.Operations.empty());
  }
  ASSERT_EQ(F.UnwindOperations.size(), 2u);
  EXPECT_EQ(F.UnwindOperations[0].Kind, UnwindOperationKind::AllocateSmall);
  EXPECT_EQ(F.UnwindOperations[1].Kind, UnwindOperationKind::PushNonVolatile);
  EXPECT_EQ(F.NativeUnwindBytes, Bytes);
}

TEST(COFFExceptionParser, V2BodyMayHaveNoEpilogueAndOperandsAreNotMarkers) {
  for (bool HasEpilogue : {false, true}) {
    BinaryImage Img = makeX64ExceptionImage();
    std::vector<uint8_t> Bytes = {2, 4, uint8_t(HasEpilogue ? 4 : 2), 0};
    if (HasEpilogue)
      Bytes.insert(Bytes.end(), {1, 0x16, 0, 6});
    Bytes.insert(Bytes.end(), {4, 1, 6, 0}); // ALLOC_LARGE 6 * 8.
    std::copy(Bytes.begin(), Bytes.end(), Img.Segments[1].Data.begin());
    const auto F = coff_loader::decodeX64ExceptionFunction(
        Img, Img.Base, 0x2000, 0x1000, 0x1040, 0x3000);
    EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Complete);
    EXPECT_EQ(F.Epilogs.size(), HasEpilogue ? 1u : 0u);
    ASSERT_EQ(F.UnwindOperations.size(), 1u);
    EXPECT_EQ(F.UnwindOperations[0].StackOffset, 48u);
  }
}

TEST(COFFExceptionParser,
     V2NonleadingEpiloguePairsDoNotRestoreOrDefineRegions) {
  for (bool HasEpilogue : {false, true}) {
    for (bool BetweenOperations : {false, true}) {
      SCOPED_TRACE(HasEpilogue);
      SCOPED_TRACE(BetweenOperations);
      BinaryImage Img = makeX64ExceptionImage();
      std::vector<uint8_t> Bytes = {2, 5, uint8_t(HasEpilogue ? 6 : 4), 0};
      if (HasEpilogue)
        Bytes.insert(Bytes.end(), {1, 0x16, 0, 6});
      Bytes.insert(Bytes.end(), {5, 0x32}); // Allocate 32 bytes.
      if (!BetweenOperations)
        Bytes.insert(Bytes.end(), {1, 0x30}); // Push rbx.
      // Neither the offset nor the flags describe a prologue operation.
      // The next slot is ignored payload, even with an unknown opcode.
      Bytes.insert(Bytes.end(), {0xfe, 0xf6, 0xa5, 0xff});
      if (BetweenOperations)
        Bytes.insert(Bytes.end(), {1, 0x30});
      std::copy(Bytes.begin(), Bytes.end(), Img.Segments[1].Data.begin());
      const auto F = coff_loader::decodeX64ExceptionFunction(
          Img, Img.Base, 0x2000, 0x1000, 0x1040, 0x3000);
      ASSERT_EQ(F.ParseStatus, ExceptionParseStatus::Complete);
      EXPECT_EQ(F.Epilogs.size(), HasEpilogue ? 1u : 0u);
      ASSERT_EQ(F.UnwindOperations.size(), 2u);
      EXPECT_EQ(F.UnwindOperations[0].Kind, UnwindOperationKind::AllocateSmall);
      EXPECT_EQ(F.UnwindOperations[0].CodeOffset, 5u);
      EXPECT_EQ(F.UnwindOperations[1].Kind,
                UnwindOperationKind::PushNonVolatile);
      EXPECT_EQ(F.UnwindOperations[1].CodeOffset, 1u);
      EXPECT_EQ(F.NativeUnwindBytes, Bytes);
    }
  }
}

TEST(COFFExceptionParser, RejectsMalformedV2EpiloguesAndPrologueOrdering) {
  const std::vector<std::vector<uint8_t>> Cases = {
      {0, 0x16, 0, 6},             // Zero length.
      {1, 0x26, 0, 6},             // Reserved header flags.
      {1, 0x16},                   // Unpaired epilogue slot.
      {1, 6, 0, 6},                // No actual epilogue.
      {4, 6, 3, 6},                // Region extends past function end.
      {4, 6, 1, 0x16},             // Distance exceeds function size.
      {4, 6, 0x3c, 6},             // Region overlaps prologue.
      {4, 0x16, 4, 6},             // Duplicate region.
      {4, 0x16, 5, 6},             // Overlapping regions.
      {1, 0x16, 0, 6, 8, 6, 0, 6}, // Offset after padding.
      {4, 0x32, 1, 6},             // Truncated nonleading marker pair.
      {1, 0x30, 0xfe, 0xf6, 0xa5, 0xff, 4,
       0x32},                             // Ordering across an ignored pair.
      {1, 0x16, 0, 6, 1, 0x30, 4, 0x32}}; // Increasing prologue offsets.
  for (const auto &Codes : Cases) {
    SCOPED_TRACE(testing::PrintToString(Codes));
    BinaryImage Img = makeX64ExceptionImage();
    auto &Bytes = Img.Segments[1].Data;
    Bytes[0] = 2;
    Bytes[1] = 8;
    Bytes[2] = Codes.size() / 2;
    std::copy(Codes.begin(), Codes.end(), Bytes.begin() + 4);
    const auto F = coff_loader::decodeX64ExceptionFunction(
        Img, Img.Base, 0x2000, 0x1000, 0x1040, 0x3000);
    EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Malformed);
  }
}

TEST(COFFExceptionParser, ReservedV2SpareCodeIsNeverCompleteMetadata) {
  BinaryImage Img = makeX64ExceptionImage();
  auto &Bytes = Img.Segments[1].Data;
  Bytes[0] = 2;
  Bytes[1] = 4;
  Bytes[2] = 3;
  Bytes[4] = 4;
  Bytes[5] = 7;
  const auto F = coff_loader::decodeX64ExceptionFunction(
      Img, Img.Base, 0x2000, 0x1000, 0x1040, 0x3000);
  EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Partial);
  ASSERT_EQ(F.UnwindOperations.size(), 1u);
  EXPECT_EQ(F.UnwindOperations[0].Kind, UnwindOperationKind::Opaque);
  EXPECT_EQ(F.UnwindOperations[0].SlotCount, 3u);
}

} // namespace
