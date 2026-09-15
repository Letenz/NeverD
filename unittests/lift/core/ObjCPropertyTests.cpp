#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCEncoding.h"
#include "neverd/loader/ObjC/ObjCMetadataJSON.h"
#include "neverd/loader/ObjC/ObjCSourceDeclarations.h"

#include "llvm/Support/Endian.h"

#include <cstring>

using namespace neverd;
namespace {
class PropertyImage {
public:
  BinaryImage Image;
  void pointer(va_t Address, va_t Value) {
    llvm::support::endian::write64le(
        Image.Segments[0].Data.data() + Address - 0x1000, Value);
    Image.MachOResolvedChainedPointerSlots.insert(Address);
  }
  void word(va_t Address, uint32_t Value) {
    llvm::support::endian::write32le(
        Image.Segments[0].Data.data() + Address - 0x1000, Value);
  }
  void string(va_t Address, const std::string &Value) {
    std::memcpy(Image.Segments[0].Data.data() + Address - 0x1000, Value.c_str(),
                Value.size() + 1);
  }
  void section(const char *Name, va_t Address, unsigned Size) {
    Section S;
    S.Name = Name;
    S.VA = Address;
    S.FileOff = Address - 0x1000;
    S.Size = S.FileSz = Size;
    S.Flags = SegmentFlags::Readable;
    Image.Sections.push_back(S);
  }
  PropertyImage() {
    Image.Arch = Arch::AArch64;
    Image.Format = BinaryFormat::MachO;
    Image.Bits = Bitness::Bits64;
    Segment Segment;
    Segment.VA = 0x1000;
    Segment.Size = Segment.FileSz = 0x4000;
    Segment.Flags = SegmentFlags::Readable;
    Segment.Data.resize(Segment.Size);
    Image.Segments.push_back(Segment);
    section("__objc_classlist", 0x2000, 8);
    section("__objc_const", 0x2100, 0x2f00);
    pointer(0x2000, 0x2200);
    pointer(0x2200, 0x2280);
    pointer(0x2220, 0x2300);
    pointer(0x22a0, 0x2380);
    word(0x2300, 2);
    word(0x2380, 3);
    pointer(0x2318, 0x2500);
    pointer(0x2398, 0x2500);
    string(0x2500, "DynamicRecord");
    string(0x2540, "eventCount");
    string(0x2580, "Tq,D,N");
    pointer(0x2340, 0x2600);
    word(0x2600, 16);
    word(0x2604, 1);
    pointer(0x2608, 0x2540);
    pointer(0x2610, 0x2580);
  }
};

TEST(ObjCProperties, DynamicAccessorsCarryTypesWithoutInventingMethods) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (const char *Type :
         {"s", "q", "f", "d", "@\"NSString\"", "^{Box=qq}"}) {
      PropertyImage F;
      F.Image.Arch = Architecture;
      F.string(0x2580, std::string("T") + Type + ",D,N");
      parseObjCMethods(F.Image);
      ASSERT_EQ(F.Image.ObjCProperties.size(), 1U);
      EXPECT_EQ(F.Image.ObjCProperties[0].Status, "supported");
      EXPECT_TRUE(F.Image.ObjCMethods.empty());
      EXPECT_TRUE(F.Image.Symbols.empty());
      auto Getter = objcSelectorSourceTypeHint(F.Image, "eventCount");
      auto Setter = objcSelectorSourceTypeHint(F.Image, "setEventCount:");
      ASSERT_TRUE(Getter);
      ASSERT_TRUE(Setter);
      size_t Offset = 0;
      auto Expected = parseObjCScalarType(Type, Offset);
      ASSERT_TRUE(Expected);
      EXPECT_EQ(Getter->ReturnType->str(), Expected->str());
      ASSERT_EQ(Setter->Parameters.size(), 3U);
      EXPECT_EQ(Setter->Parameters[2].Type->str(), Expected->str());
      EXPECT_EQ(Getter->Parameters.size(), 2U);
      EXPECT_EQ(Setter->ReturnType->Kind, NdTypeKind::Void);
      EXPECT_EQ(Getter->ReturnLocation.RegisterOffset,
                Expected->Kind == NdTypeKind::Float
                    ? getTargetRegInfo(Architecture).FPReturnReg
                    : getTargetRegInfo(Architecture).IntReturnReg);
    }
}

TEST(ObjCProperties, CustomAccessorsAndReadonlyKeepDeclaredSelectorArity) {
  for (bool ReadOnly : {false, true}) {
    PropertyImage F;
    F.string(0x2580, ReadOnly ? "Ti,R,GcurrentCount"
                              : "Ti,GcurrentCount,SupdateCount:");
    parseObjCMethods(F.Image);
    ASSERT_EQ(F.Image.ObjCProperties.size(), 1U);
    EXPECT_TRUE(objcSelectorSourceTypeHint(F.Image, "currentCount"));
    EXPECT_FALSE(objcSelectorSourceTypeHint(F.Image, "eventCount"));
    EXPECT_FALSE(objcSelectorSourceTypeHint(F.Image, "setEventCount:"));
    EXPECT_EQ(bool(objcSelectorSourceTypeHint(F.Image, "updateCount:")),
              !ReadOnly);
  }
}

TEST(ObjCProperties, OptionalPropertyFlagsRetainTheAccessorContract) {
  PropertyImage F;
  // Clang emits this attribute for @optional protocol properties, including
  // copies of their declarations attached to a conforming class.
  F.string(0x2580, "Tq,?,R,N");
  parseObjCMethods(F.Image);
  ASSERT_EQ(F.Image.ObjCProperties.size(), 1U);
  const auto &P = F.Image.ObjCProperties[0];
  EXPECT_TRUE(P.IsOptional);
  EXPECT_TRUE(P.ReadOnly);
  EXPECT_EQ(P.Status, "supported");
  EXPECT_TRUE(objcSelectorSourceTypeHint(F.Image, "eventCount"));
  EXPECT_FALSE(objcSelectorSourceTypeHint(F.Image, "setEventCount:"));
}

TEST(ObjCProperties, ExternalCategoryPropertiesAreReportedWithoutAClassBody) {
  PropertyImage F;
  F.pointer(0x2000, 0);
  F.section("__objc_catlist", 0x2010, 8);
  F.section("__objc_nlcatlist", 0x2018, 8);
  F.pointer(0x2010, 0x2700);
  F.pointer(0x2018, 0x2700);
  F.pointer(0x2700, 0x2780);
  F.string(0x2780, "Counters");
  F.Image.DyldBindSlots[0x2708] = {"_OBJC_CLASS_$_ExternalRecord", 0};
  F.pointer(0x2728, 0x2600);
  parseObjCMethods(F.Image);
  EXPECT_TRUE(F.Image.ObjCClasses.empty());
  EXPECT_TRUE(F.Image.ObjCMethods.empty());
  ASSERT_EQ(F.Image.ObjCProperties.size(), 1U);
  EXPECT_EQ(F.Image.ObjCProperties[0].ClassName, "ExternalRecord");
  EXPECT_TRUE(objcSelectorSourceTypeHint(F.Image, "eventCount"));
  auto JSON = objcMetadataJSON(F.Image);
  EXPECT_EQ(JSON.getString("status"), "recovered");
}

TEST(ObjCProperties,
     ConflictingAndMalformedDeclarationsRemainNegativeEvidence) {
  for (const char *Attributes :
       {"Tf,D", "Tq,Tq", "Tq,X", "Tq,R,SsetEventCount:", "Tq,WC",
        "T{Pair=qq},N", "Tv", "Tq,C,&", "T@\"Broken,N", "Tq,Sbad:setter:"}) {
    PropertyImage F;
    F.string(0x2580, Attributes);
    parseObjCMethods(F.Image);
    ObjCMethod M;
    M.Selector = "eventCount";
    M.TypeHint = parseObjCMethodEncoding(M.Selector, "q@:");
    F.Image.ObjCMethods.push_back(M);
    EXPECT_FALSE(objcSelectorSourceTypeHint(F.Image, "eventCount"))
        << Attributes;
  }
}

TEST(ObjCProperties, InvalidListsAndUnresolvedPointersNeverProvideHints) {
  for (unsigned Case = 0; Case != 7; ++Case) {
    PropertyImage F;
    if (Case == 0)
      F.word(0x2600, 24);
    if (Case == 1)
      F.word(0x2604, 65537);
    if (Case == 2)
      F.pointer(0x2340, UINT64_MAX);
    if (Case == 3)
      F.pointer(0x2608, 0x9000);
    if (Case == 4) {
      F.Image.MachOHasChainedFixups = true;
      F.Image.MachOResolvedChainedPointerSlots.erase(0x2610);
    }
    if (Case == 5)
      F.Image.ImportPtrSlots[0x2340] = "_foreign_property_list";
    if (Case == 6)
      F.Image.Sections[1].FileSz = 0x500;
    parseObjCMethods(F.Image);
    EXPECT_FALSE(objcSelectorSourceTypeHint(F.Image, "eventCount")) << Case;
    EXPECT_FALSE(F.Image.ObjCMetadataDiagnostics.empty()) << Case;
  }
}

TEST(ObjCProperties, CategoryClassPropertiesRequireTheImageLayoutFlag) {
  for (bool Extended : {false, true}) {
    PropertyImage F;
    F.pointer(0x2340, 0);
    F.section("__objc_catlist", 0x2010, 8);
    F.section("__objc_imageinfo", 0x2020, 8);
    F.word(0x2024, Extended ? 64 : 0);
    F.pointer(0x2010, 0x2700);
    F.pointer(0x2700, 0x2780);
    F.string(0x2780, "Counters");
    F.pointer(0x2708, 0x2200);
    F.pointer(0x2730, 0x2600);
    parseObjCMethods(F.Image);
    EXPECT_EQ(F.Image.ObjCProperties.size(), Extended ? 1U : 0U);
    if (Extended) {
      const auto &P = F.Image.ObjCProperties[0];
      EXPECT_EQ(P.Owner, ObjCProperty::OwnerKind::Category);
      EXPECT_TRUE(P.IsClassProperty);
      EXPECT_EQ(P.OwnerName, "Counters");
      EXPECT_EQ(P.ClassName, "DynamicRecord");
      EXPECT_TRUE(objcSelectorSourceTypeHint(F.Image, "eventCount"));
    }
  }
}

TEST(ObjCProperties, ProtocolPropertiesFollowSizeAndInheritanceValidity) {
  for (bool Extended : {false, true})
    for (bool Cycle : {false, true}) {
      PropertyImage F;
      F.pointer(0x2340, 0);
      F.section("__objc_protolist", 0x2010, 8);
      F.pointer(0x2010, 0x2800);
      F.pointer(0x2808, 0x2500);
      F.word(0x2840, Extended ? 96 : 72);
      F.pointer(Extended ? 0x2858 : 0x2838, 0x2600);
      F.pointer(0x2858,
                0x2600); // Optional bytes are ignored in the short record.
      if (Cycle) {
        F.pointer(0x2810, 0x2900);
        F.pointer(0x2900, 1);
        F.pointer(0x2908, 0x2800);
      }
      parseObjCMethods(F.Image);
      ASSERT_EQ(F.Image.ObjCProperties.size(), 1U);
      const auto &P = F.Image.ObjCProperties[0];
      EXPECT_EQ(P.Owner, ObjCProperty::OwnerKind::Protocol);
      EXPECT_EQ(P.IsClassProperty, Extended);
      EXPECT_EQ(bool(objcSelectorSourceTypeHint(F.Image, "eventCount")),
                !Cycle);
    }
}

TEST(ObjCProperties, ReloadAndJSONKeepPropertyIdentitySeparateFromMethods) {
  PropertyImage F;
  F.pointer(0x2340, 0);
  F.pointer(0x23c0, 0x2600);
  parseObjCMethods(F.Image);
  parseObjCMethods(F.Image);
  ASSERT_EQ(F.Image.ObjCProperties.size(), 1U);
  EXPECT_TRUE(F.Image.ObjCProperties[0].IsClassProperty);
  auto JSON = objcMetadataJSON(F.Image);
  ASSERT_NE(JSON.getArray("properties"), nullptr);
  EXPECT_EQ(JSON.getArray("properties")->size(), 1U);
  EXPECT_TRUE(F.Image.ObjCMethods.empty());
  F.pointer(0x2000, 0);
  parseObjCMethods(F.Image);
  EXPECT_TRUE(F.Image.ObjCProperties.empty());
  EXPECT_FALSE(objcSelectorSourceTypeHint(F.Image, "eventCount"));
}
} // namespace
