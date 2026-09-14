#include "../../../lib/sdk/capi/ObjCSourceBindings.h"
#include "gtest/gtest.h"

#include "neverd/loader/ObjC/ObjCMethods.h"

#include "llvm/Support/Endian.h"

#include <cstring>

using namespace neverd;
namespace {
struct StorageImage {
  BinaryImage Image;
  void word(va_t Address, uint32_t Value) {
    llvm::support::endian::write32le(
        Image.Segments[0].Data.data() + Address - 0x1000, Value);
  }
  void pointer(va_t Address, uint64_t Value) {
    llvm::support::endian::write64le(
        Image.Segments[0].Data.data() + Address - 0x1000, Value);
  }
  void string(va_t Address, const char *Text) {
    std::memcpy(Image.Segments[0].Data.data() + Address - 0x1000, Text,
                std::strlen(Text) + 1);
  }
  StorageImage() {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Segment Mapping;
    Mapping.VA = Mapping.FileOff = 0x1000;
    Mapping.Size = Mapping.FileSz = 0x1000;
    Mapping.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Mapping.Data.resize(0x1000);
    Image.Segments.push_back(Mapping);
    Section Records;
    Records.Name = "__objc_const";
    Records.VA = Records.FileOff = 0x1000;
    Records.Size = Records.FileSz = 0x1000;
    Records.Flags = Mapping.Flags;
    Image.Sections.push_back(Records);
    ObjCClass Class;
    Class.Name = "StoredValues";
    Class.Address = 0x1100;
    Image.ObjCClasses.push_back(Class);
    pointer(0x1120, 0x1202); // Stable Swift class, with two stored fields.
    word(0x1204, 8);
    word(0x1208, 32);
    pointer(0x1230, 0x1300);
    word(0x1300, 32);
    word(0x1304, 2);
    for (unsigned Index = 0; Index < 2; ++Index) {
      const va_t Entry = 0x1308 + Index * 32;
      pointer(Entry, 0x1400 + Index * 8);
      pointer(Entry + 8, 0x1500 + Index * 64);
      pointer(Entry + 16, 0x1520 + Index * 64);
      word(Entry + 24, 3);
      word(Entry + 28, Index ? 16 : 8);
      pointer(0x1400 + Index * 8, Index ? 16 : 8);
    }
    string(0x1500, "first");
    string(0x1520, "d");
    string(0x1540, "opaque");
    string(0x1560, "");
  }
};

TEST(ObjCStorage, StableSwiftPreservesWideOffsetsAndAbsentFieldTypes) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    StorageImage Fixture;
    Fixture.Image.Arch = Architecture;
    parseObjCStorage(Fixture.Image);
    const auto &Class = Fixture.Image.ObjCClasses.front();
    ASSERT_EQ(Class.IvarStatus, "recovered");
    ASSERT_EQ(Class.Ivars.size(), 2U);
    EXPECT_EQ(Class.Ivars[0].TypeEncoding, "d");
    EXPECT_TRUE(Class.Ivars[1].TypeEncoding.empty());
    EXPECT_EQ(Class.Ivars[1].Size, 16U);
    ASSERT_EQ(Fixture.Image.ObjCSourceReferences.size(), 2U);
    EXPECT_EQ(Fixture.Image.ObjCSourceReferences.at(0x1400).Size, 8U);
    for (unsigned Width : {4U, 8U}) {
      HighFunc Function;
      HighStmt Return;
      Return.Kind = StmtKind::Return;
      Return.RetVal = HighExpr::makeLoad(HighExpr::makeConst(0x1400, 8),
                                         NdType::makeInt(Width));
      Function.Body.push_back(Return);
      auto Bound = sdk::bindObjCSourceReferences(Function, Fixture.Image);
      EXPECT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      ASSERT_TRUE(Bound.Function.Body[0].RetVal->SourceCallHint);
      EXPECT_EQ(Bound.Function.Body[0].RetVal->SourceCallHint->CallKind,
                SourceCallTypeHint::Kind::RuntimeIvarOffset);
      EXPECT_EQ(Bound.Function.Body[0].RetVal->SourceCallHint->TargetName,
                "first");
    }
  }
}

TEST(ObjCStorage, Arm64ObjCOffsetsDoNotBorrowTheFollowingWord) {
  StorageImage Fixture;
  Fixture.pointer(0x1120, 0x1200);
  Fixture.string(0x1560, "q");
  Fixture.word(0x1344, 8); // Second ivar size, at entry + 28.
  Fixture.word(0x1404, UINT32_MAX);
  parseObjCStorage(Fixture.Image);
  ASSERT_EQ(Fixture.Image.ObjCClasses.front().IvarStatus, "recovered");
  EXPECT_EQ(Fixture.Image.ObjCSourceReferences.at(0x1400).Size, 4U);
  HighFunc Function;
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal =
      HighExpr::makeLoad(HighExpr::makeConst(0x1400, 8), NdType::makeInt(8));
  Function.Body.push_back(Return);
  EXPECT_FALSE(sdk::bindObjCSourceReferences(Function, Fixture.Image)
                   .Limitation.empty());
}

TEST(ObjCStorage, UnprovenEmptyTypesAndTruncatedWideOffsetsRemainRejected) {
  for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
    StorageImage Fixture;
    if (Mutation == 0)
      Fixture.pointer(0x1120, 0x1200);
    if (Mutation == 1)
      Fixture.pointer(0x1120, 0x1201); // Legacy Swift is a separate ABI.
    if (Mutation == 2)
      Fixture.word(0x1404, 1); // Must not truncate the live 64-bit offset.
    if (Mutation == 3)
      Fixture.pointer(0x1338, 0); // Null type pointer is not an empty string.
    if (Mutation == 4)
      Fixture.string(0x1540, ""); // Field names always require identity.
    if (Mutation == 5)
      Fixture.pointer(0x1308, 0x1ffc); // Only four offset bytes are mapped.
    parseObjCStorage(Fixture.Image);
    EXPECT_EQ(Fixture.Image.ObjCClasses.front().IvarStatus, "unresolved")
        << Mutation;
    EXPECT_TRUE(Fixture.Image.ObjCSourceReferences.empty()) << Mutation;
  }
}
} // namespace
