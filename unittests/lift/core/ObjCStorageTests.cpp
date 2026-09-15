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

TEST(ObjCStorage, ProtocolSlotsRequireCompleteUniqueLocalRuntimeDeclarations) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Case = 0; Case < 11; ++Case) {
      SCOPED_TRACE(Case);
      StorageImage F;
      F.Image.Arch = Architecture;
      F.Image.Sections[0].Size = F.Image.Sections[0].FileSz = 0x800;
      Section References;
      References.Name = "__objc_protorefs";
      References.VA = References.FileOff = 0x1800;
      References.Size = References.FileSz = 8;
      References.Flags = SegmentFlags::Readable;
      F.Image.Sections.push_back(References);
      ObjCProtocol Protocol;
      Protocol.Address = 0x1700;
      Protocol.Name = "ValueProtocol";
      Protocol.Status = "recovered";
      F.Image.ObjCProtocols.push_back(Protocol);
      F.pointer(0x1800, 0x1700);
      switch (Case) {
      case 0:
        break;
      case 1:
        F.Image.ObjCProtocols[0].Status = "invalid_metadata";
        break;
      case 2:
        F.Image.ObjCProtocols[0].Status = "invalid_inheritance";
        break;
      case 3:
        F.Image.ObjCProtocols.clear();
        break;
      case 4:
        Protocol.Address += 8;
        F.Image.ObjCProtocols.push_back(Protocol);
        break;
      case 5:
        Protocol.Name = "Other";
        F.Image.ObjCProtocols.push_back(Protocol);
        break;
      case 6:
        F.Image.MachOHasChainedFixups = true;
        break;
      case 7:
        F.Image.ImportPtrSlots[0x1800] = "_OBJC_PROTOCOL_$_ValueProtocol";
        break;
      case 8:
        F.Image.ConflictingImportStorageSlots.insert(0x1800);
        break;
      case 9:
        F.Image.Sections.back().FileSz = 4;
        break;
      case 10:
        F.Image.Sections.back().Flags =
            SegmentFlags::Readable | SegmentFlags::Executable;
        break;
      }
      parseObjCStorage(F.Image);
      const auto Found = F.Image.ObjCSourceReferences.find(0x1800);
      if (Case) {
        EXPECT_EQ(Found, F.Image.ObjCSourceReferences.end());
        continue;
      }
      ASSERT_NE(Found, F.Image.ObjCSourceReferences.end());
      EXPECT_EQ(Found->second.TheKind, ObjCSourceReference::Kind::Protocol);
      EXPECT_EQ(Found->second.Name, "ValueProtocol");
      HighFunc Function;
      HighStmt Return;
      Return.Kind = StmtKind::Return;
      Return.RetVal = HighExpr::makeLoad(HighExpr::makeConst(0x1800, 8),
                                         NdType::makePtr(NdType::makeVoid()));
      Function.Body = {Return};
      const auto Bound = sdk::bindObjCSourceReferences(Function, F.Image);
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      ASSERT_TRUE(Bound.Function.Body[0].RetVal->SourceCallHint);
      EXPECT_EQ(Bound.RuntimeProtocols, std::set<std::string>{"ValueProtocol"});
      EXPECT_TRUE(sdk::objcSourceCallBound(*Bound.Function.Body[0].RetVal,
                                           F.Image, {}));
      auto Hint = std::make_shared<SourceCallTypeHint>(
          *Bound.Function.Body[0].RetVal->SourceCallHint);
      Hint->TargetName = "Other";
      Bound.Function.Body[0].RetVal->SourceCallHint = Hint;
      EXPECT_FALSE(sdk::objcSourceCallBound(*Bound.Function.Body[0].RetVal,
                                            F.Image, {}));
    }
  }
}

TEST(ObjCStorage, ImportedClassSlotsDoNotRequireAClassReferenceSection) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (bool Metaclass : {false, true}) {
      for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
        StorageImage Fixture;
        auto &Image = Fixture.Image;
        Image.Arch = Architecture;
        Image.ObjCClasses.clear();
        Image.Sections.front().Name = "__got";
        const std::string Name =
            Metaclass ? "_OBJC_METACLASS_$_Example" : "_OBJC_CLASS_$_Example";
        Image.ImportPtrSlots[0x1800] = Name;
        Image.DyldBindSlots[0x1800] = {Name, 0};
        if (Mutation == 1)
          Image.DyldBindSlots[0x1800].WeakImport = true;
        if (Mutation == 2)
          Image.DyldBindSlots[0x1800].Addend = 8;
        if (Mutation == 3)
          Image.ConflictingImportStorageSlots.insert(0x1800);
        if (Mutation == 4)
          Image.ImportStorageSlots[0x1800] = {"_other", 0};
        if (Mutation == 5)
          Image.Sections.front().Flags =
              Image.Sections.front().Flags | SegmentFlags::Executable;
        if (Mutation == 6)
          Image.Sections.front().FileSz = 0x804;
        if (Mutation == 7)
          Image.DyldBindSlots[0x1800].Name = "_different";
        parseObjCStorage(Image);
        const auto Found = Image.ObjCSourceReferences.find(0x1800);
        if (Mutation) {
          EXPECT_EQ(Found, Image.ObjCSourceReferences.end()) << Mutation;
          continue;
        }
        ASSERT_NE(Found, Image.ObjCSourceReferences.end());
        EXPECT_EQ(Found->second.Name, "Example");
        EXPECT_EQ(Found->second.TheKind,
                  Metaclass ? ObjCSourceReference::Kind::Metaclass
                            : ObjCSourceReference::Kind::Class);
        HighFunc Function;
        HighStmt Return;
        Return.Kind = StmtKind::Return;
        Return.RetVal = HighExpr::makeLoad(HighExpr::makeConst(0x1800, 8),
                                           NdType::makePtr(NdType::makeVoid()));
        Function.Body.push_back(Return);
        const auto Bound = sdk::bindObjCSourceReferences(Function, Image);
        EXPECT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
        ASSERT_TRUE(Bound.Function.Body[0].RetVal->SourceCallHint);
        EXPECT_EQ(Bound.Function.Body[0].RetVal->SourceCallHint->TargetName,
                  "Example");
      }
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
