//===- LocalTypesTests.cpp - The local type library and nesting -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "TypeShape.h"

#include "neverd/types/LocalTypes.h"

#include <gtest/gtest.h>
#include <optional>

using namespace neverd::types;
using neverd::types::test::shapeOf;

namespace {

/// A member as MSVC lists it.
FlatMember flat(std::string Name, Builtin Type, uint64_t Byte,
                DataModel Model = DataModel::LLP64) {
  FlatMember Member;
  Member.Name = std::move(Name);
  Member.Type = makeBuiltin(Type);
  Member.BitOffset = Byte * 8;
  Member.StorageBitOffset = Byte * 8;
  Member.StorageBits = builtinBytes(Type, Model, defaultRecordABI(Model)) * 8;
  Member.Alignment = builtinBytes(Type, Model, defaultRecordABI(Model));
  return Member;
}

/// \p Count elements of \p Type at \p Byte.
FlatMember array(std::string Name, Builtin Type, uint64_t Count,
                 uint64_t Byte) {
  FlatMember Member = flat(std::move(Name), Type, Byte);
  Member.Type = makeArray(makeBuiltin(Type), Count);
  Member.StorageBits *= Count;
  return Member;
}

/// A bitfield in a storage unit of \p Type at \p Byte.
FlatMember bits(std::string Name, Builtin Type, uint64_t Byte,
                uint32_t Position, uint32_t Width) {
  FlatMember Member = flat(std::move(Name), Type, Byte);
  Member.BitOffset += Position;
  Member.BitWidth = Width;
  return Member;
}

RecordDef nested(const std::vector<FlatMember> &Flat, bool Union = false) {
  RecordDef Record;
  Record.Union = Union;
  Record.Members = nestFlatMembers(Flat, Union);
  return Record;
}

/// The nesting flattens back to exactly what it was built from.
void expectRoundTrip(const RecordDef &Record,
                     const std::vector<FlatMember> &Flat) {
  const std::vector<FlatMember> Back = flattenMembers(Record);
  ASSERT_EQ(Back.size(), Flat.size());
  for (size_t I = 0; I < Flat.size(); ++I) {
    EXPECT_EQ(Back[I].Name, Flat[I].Name);
    EXPECT_EQ(Back[I].BitOffset, Flat[I].BitOffset) << Flat[I].Name;
    EXPECT_EQ(Back[I].BitWidth, Flat[I].BitWidth) << Flat[I].Name;
  }
}

} // namespace

TEST(LocalTypes, BuiltinSizesFollowTheDataModelAndTheRecordABI) {
  EXPECT_EQ(
      builtinBytes(Builtin::ULong, DataModel::LLP64, RecordABI::Microsoft), 4u);
  EXPECT_EQ(builtinBytes(Builtin::ULong, DataModel::LP64, RecordABI::Itanium),
            8u);
  EXPECT_EQ(pointerBytes(DataModel::ILP32), 4u);
  // wchar_t and long double are as large as the target's compilers make
  // them, which its data model does not say.
  const auto sizes = [](DataModel Model, RecordABI ABI) {
    return std::to_string(builtinBytes(Builtin::WChar, Model, ABI)) + " " +
           std::to_string(builtinBytes(Builtin::LongDouble, Model, ABI));
  };
  EXPECT_EQ(sizes(DataModel::LLP64, RecordABI::Microsoft), "2 8");
  EXPECT_EQ(sizes(DataModel::ILP32, RecordABI::Microsoft), "2 8");
  EXPECT_EQ(sizes(DataModel::LP64, RecordABI::Itanium), "4 16");
  EXPECT_EQ(sizes(DataModel::LP64, RecordABI::AppleARM64), "4 8");
  EXPECT_EQ(sizes(DataModel::ILP32, RecordABI::I386SystemV), "4 12");
  EXPECT_EQ(sizes(DataModel::ILP32, RecordABI::AndroidI386), "4 8");
  EXPECT_EQ(sizes(DataModel::ILP32, RecordABI::AAPCS32), "4 8");
  EXPECT_EQ(sizes(DataModel::LLP64, RecordABI::MinGW64), "2 16");
  EXPECT_EQ(sizes(DataModel::ILP32, RecordABI::MinGW32), "2 12");
  // A MinGW program joins the Windows libraries, whose records lay out as
  // its own do.
  EXPECT_EQ(libraryRecordABIOf(RecordABI::MinGW64), RecordABI::Microsoft);
  EXPECT_EQ(libraryRecordABIOf(RecordABI::MinGW32), RecordABI::Microsoft);
  EXPECT_EQ(libraryRecordABIOf(RecordABI::I386SystemV), RecordABI::I386SystemV);
  TypeLibrary ARM(DataModel::ILP32);
  ARM.setRecordABI(RecordABI::AAPCS32);
  EXPECT_EQ(ARM.builtinBytes(Builtin::LongDouble), 8u);
  EXPECT_EQ(ARM.sizeOf(*makeBuiltin(Builtin::WChar)), 4u);
  EXPECT_STREQ(builtinInfo(Builtin::ULong).Windows, "ULONG");
  EXPECT_STREQ(builtinInfo(Builtin::ULongLong).Keywords, "unsigned long long");
}

TEST(LocalTypes, ScalarsAlignAsTheirRecordABISays) {
  // The i386 System V ABI aligns eight-byte scalars to four in a record,
  // Microsoft's compilers and ARM's to eight; the data model says neither.
  const auto alignment = [](DataModel Model, std::optional<RecordABI> ABI,
                            Builtin Type) {
    TypeLibrary Library(Model);
    if (ABI)
      Library.setRecordABI(*ABI);
    TypeExpr Scalar;
    Scalar.Kind = TypeExprKind::Builtin;
    Scalar.TheBuiltin = Type;
    return Library.alignOf(Scalar);
  };
  EXPECT_EQ(TypeLibrary(DataModel::ILP32).recordABI(), RecordABI::Microsoft);
  EXPECT_EQ(TypeLibrary(DataModel::LP64).recordABI(), RecordABI::Itanium);
  EXPECT_EQ(alignment(DataModel::ILP32, std::nullopt, Builtin::LongLong), 8u);
  EXPECT_EQ(
      alignment(DataModel::ILP32, RecordABI::I386SystemV, Builtin::LongLong),
      4u);
  EXPECT_EQ(
      alignment(DataModel::ILP32, RecordABI::I386SystemV, Builtin::Double), 4u);
  EXPECT_EQ(
      alignment(DataModel::ILP32, RecordABI::I386SystemV, Builtin::LongDouble),
      4u);
  EXPECT_EQ(alignment(DataModel::ILP32, RecordABI::AAPCS32, Builtin::Double),
            8u);
  EXPECT_EQ(alignment(DataModel::ILP32, RecordABI::I386SystemV, Builtin::Int),
            4u);
  // Microsoft's long double is a double, and the 64-bit System V targets'
  // the x87's in sixteen bytes, aligned as large.
  EXPECT_EQ(alignment(DataModel::ILP32, std::nullopt, Builtin::LongDouble), 8u);
  EXPECT_EQ(
      alignment(DataModel::LLP64, RecordABI::MinGW64, Builtin::LongDouble),
      16u);
  // MinGW's x86 aligns its twelve bytes to four, and a double to eight.
  EXPECT_EQ(
      alignment(DataModel::ILP32, RecordABI::MinGW32, Builtin::LongDouble), 4u);
  EXPECT_EQ(alignment(DataModel::ILP32, RecordABI::MinGW32, Builtin::Double),
            8u);
  EXPECT_EQ(alignment(DataModel::LP64, std::nullopt, Builtin::LongDouble), 16u);
  EXPECT_EQ(layoutRulesOf(RecordABI::I386SystemV), LayoutRules::Itanium);
  EXPECT_EQ(recordABIName(RecordABI::I386SystemV), "i386-sysv");
}

TEST(LocalTypes, ReplacingATypeKeepsItsOrdinal) {
  TypeLibrary Library;
  LocalType A;
  A.Name = "_A";
  A.Record = std::make_shared<RecordDef>();
  LocalType B;
  B.Name = "_B";
  B.Record = std::make_shared<RecordDef>();
  EXPECT_EQ(Library.define(A), 1u);
  EXPECT_EQ(Library.define(B), 2u);
  auto Bigger = std::make_shared<RecordDef>();
  Bigger->Size = 16;
  A.Record = Bigger;
  EXPECT_EQ(Library.define(A), 1u);
  EXPECT_EQ(Library.find("_A")->Record->Size, 16u);
  EXPECT_TRUE(Library.remove("_A"));
  EXPECT_EQ(Library.find("_A"), nullptr);
  EXPECT_EQ(Library.byOrdinal(1), nullptr);
  ASSERT_EQ(Library.types().size(), 1u);
  EXPECT_EQ(Library.types().front()->Name, "_B");
}

TEST(LocalTypes, SizesResolveNamesAndTypedefs) {
  TypeLibrary Library;
  LocalType Entry;
  Entry.Name = "_LIST_ENTRY";
  auto Record = std::make_shared<RecordDef>();
  Record->Size = 16;
  Record->Members = {
      {"Flink", makePointer(makeNamed("_LIST_ENTRY", TypeKind::Struct), 8), 0},
      {"Blink", makePointer(makeNamed("_LIST_ENTRY", TypeKind::Struct), 8),
       64}};
  Entry.Record = Record;
  Library.define(Entry);
  LocalType Alias;
  Alias.Name = "LIST_ENTRY";
  Alias.Kind = TypeKind::Typedef;
  Alias.Target = makeNamed("_LIST_ENTRY", TypeKind::Struct);
  Library.define(Alias);

  EXPECT_EQ(Library.resolve("LIST_ENTRY")->Name, "_LIST_ENTRY");
  EXPECT_EQ(Library.sizeOf(*makeNamed("LIST_ENTRY", TypeKind::Typedef)), 16u);
  EXPECT_EQ(
      Library.sizeOf(*makeArray(makeNamed("_LIST_ENTRY", TypeKind::Struct), 3)),
      48u);
  EXPECT_EQ(Library.alignOf(*makeNamed("_LIST_ENTRY", TypeKind::Struct)), 8u);
  EXPECT_FALSE(Library.sizeOf(*makeNamed("_MISSING", TypeKind::Struct)));
}

TEST(LocalTypesNesting, SequentialMembersStayFlat) {
  const std::vector<FlatMember> Flat = {flat("Type", Builtin::UShort, 0),
                                        flat("Size", Builtin::UShort, 2),
                                        flat("Next", Builtin::ULongLong, 8)};
  const RecordDef Record = nested(Flat);
  EXPECT_EQ(shapeOf(Record), "struct{Type@0 Size@2 Next@8}");
  expectRoundTrip(Record, Flat);
}

TEST(LocalTypesNesting, AScalarOverItsBitsIsAUnion) {
  // _EPROCESS: union { ULONG Flags2; struct { ULONG A:1; ULONG B:1; ... }; }
  const std::vector<FlatMember> Flat = {
      flat("Flags2", Builtin::ULong, 0), bits("A", Builtin::ULong, 0, 0, 1),
      bits("B", Builtin::ULong, 0, 1, 1), bits("C", Builtin::ULong, 0, 2, 30),
      flat("After", Builtin::ULong, 4)};
  const RecordDef Record = nested(Flat);
  EXPECT_EQ(
      shapeOf(Record),
      "struct{union@0{Flags2@0 struct@0{A@0b:1 B@1b:1 C@2b:30}} After@4}");
  expectRoundTrip(Record, Flat);
}

TEST(LocalTypesNesting, SiblingFlagUnionsStayApart) {
  // _KTHREAD's flag unions at 0x74 and 0x78, which IDA merges into one.
  const std::vector<FlatMember> Flat = {
      flat("Header", Builtin::ULongLong, 0),
      bits("AutoBoostActive", Builtin::ULong, 8, 0, 1),
      bits("ReadyTransition", Builtin::ULong, 8, 1, 1),
      bits("MiscSpare", Builtin::ULong, 8, 2, 30),
      flat("MiscFlags", Builtin::Long, 8),
      bits("ThreadFlagsSpare", Builtin::ULong, 12, 0, 2),
      bits("AutoAlignment", Builtin::ULong, 12, 2, 1),
      bits("ThreadReserved", Builtin::ULong, 12, 3, 29),
      flat("ThreadFlags", Builtin::Long, 12),
      flat("Tail", Builtin::UChar, 16)};
  const RecordDef Record = nested(Flat);
  EXPECT_EQ(shapeOf(Record),
            "struct{Header@0 "
            "union@8{struct@0{AutoBoostActive@0b:1 ReadyTransition@1b:1 "
            "MiscSpare@2b:30} MiscFlags@0} "
            "union@12{struct@0{ThreadFlagsSpare@0b:2 AutoAlignment@2b:1 "
            "ThreadReserved@3b:29} ThreadFlags@0} Tail@16}");
  expectRoundTrip(Record, Flat);
}

TEST(LocalTypesNesting, AStructAlternativeTakesTheMembersInsideTheUnion) {
  // union { ULONGLONG Value; struct { ULONG Low; ULONG High; }; }
  const std::vector<FlatMember> Flat = {
      flat("Value", Builtin::ULongLong, 0), flat("Low", Builtin::ULong, 0),
      flat("High", Builtin::ULong, 4), flat("Next", Builtin::ULong, 8)};
  const RecordDef Record = nested(Flat);
  EXPECT_EQ(shapeOf(Record),
            "struct{union@0{Value@0 struct@0{Low@0 High@4}} Next@8}");
  expectRoundTrip(Record, Flat);
}

TEST(LocalTypesNesting, AUnionRecordSplitsAtItsStart) {
  // _LARGE_INTEGER: struct { LowPart; HighPart; }; QuadPart.
  const std::vector<FlatMember> Flat = {flat("LowPart", Builtin::ULong, 0),
                                        flat("HighPart", Builtin::Long, 4),
                                        flat("QuadPart", Builtin::LongLong, 0)};
  const RecordDef Record = nested(Flat, /*Union=*/true);
  EXPECT_EQ(shapeOf(Record),
            "union{struct@0{LowPart@0 HighPart@4} QuadPart@0}");
  expectRoundTrip(Record, Flat);
}

TEST(LocalTypesNesting, ABitfieldRunResetsIntoAnotherAlternative) {
  // union { struct { ULONG A:16; ULONG B:16; }; struct { ULONG C:8; ... }; }
  const std::vector<FlatMember> Flat = {
      bits("A", Builtin::ULong, 0, 0, 16), bits("B", Builtin::ULong, 0, 16, 16),
      bits("C", Builtin::ULong, 0, 0, 8), bits("D", Builtin::ULong, 0, 8, 24)};
  const RecordDef Record = nested(Flat);
  EXPECT_EQ(shapeOf(Record), "struct{union@0{struct@0{A@0b:16 B@16b:16} "
                             "struct@0{C@0b:8 D@8b:24}}}");
  expectRoundTrip(Record, Flat);
}

TEST(LocalTypesNesting, AnOverlapWithoutACommonStartIsPadded) {
  // Y overlaps the upper half of X without beginning where X does.
  const std::vector<FlatMember> Flat = {flat("X", Builtin::ULongLong, 0),
                                        flat("Y", Builtin::ULong, 4)};
  const RecordDef Record = nested(Flat);
  EXPECT_EQ(shapeOf(Record), "struct{union@0{X@0 struct@0{Y@4}}}");
  expectRoundTrip(Record, Flat);
}

TEST(LocalTypesNesting, NestedUnionInsideAStructAlternative) {
  // union { struct { A; union { B; C; }; }; D; }
  const std::vector<FlatMember> Flat = {
      flat("D", Builtin::ULongLong, 0), flat("A", Builtin::ULong, 0),
      flat("B", Builtin::ULong, 4), flat("C", Builtin::Float, 4)};
  const RecordDef Record = nested(Flat);
  EXPECT_EQ(shapeOf(Record),
            "struct{union@0{D@0 struct@0{A@0 union@4{B@0 C@0}}}}");
  expectRoundTrip(Record, Flat);
}

TEST(LocalTypesNesting, AMemberReachingBackTakesTheMembersItOverlaps) {
  // struct { union { struct { Kernel; Special; }; Combined; }; }, as
  // _KTHREAD has CombinedApcDisable over two SHORTs.
  const std::vector<FlatMember> Flat = {
      flat("Fill", Builtin::ULong, 0), flat("Kernel", Builtin::Short, 4),
      flat("Special", Builtin::Short, 6), flat("Combined", Builtin::ULong, 4)};
  const RecordDef Record = nested(Flat);
  EXPECT_EQ(shapeOf(Record), "struct{Fill@0 union@4{struct@0{Kernel@0 "
                             "Special@2} Combined@0}}");
  expectRoundTrip(Record, Flat);
}

TEST(LocalTypesNesting, AMemberReachingBackOverAGapTakesEverythingBetween) {
  // C lies inside A, after B: the union holds all three in order.
  const std::vector<FlatMember> Flat = {flat("A", Builtin::ULong, 0),
                                        flat("B", Builtin::ULong, 8),
                                        flat("C", Builtin::UShort, 2)};
  const RecordDef Record = nested(Flat);
  EXPECT_EQ(shapeOf(Record),
            "struct{union@0{struct@0{A@0 B@8} struct@0{C@2}}}");
  expectRoundTrip(Record, Flat);
}

TEST(LocalTypesNesting, LayoutProblemsNameTheMember) {
  TypeLibrary Library;
  RecordDef Record;
  Record.Size = 8;
  Member A;
  A.Name = "A";
  A.Type = makeBuiltin(Builtin::ULong);
  Member B = A;
  B.Name = "B";
  B.BitOffset = 16;
  Record.Members = {A, B};
  EXPECT_EQ(layoutProblem(Library, Record), "B overlaps the member before it");
  Record.Members[1].BitOffset = 48;
  EXPECT_EQ(layoutProblem(Library, Record), "B reaches past its record");
  Record.Members[1].BitOffset = 32;
  EXPECT_EQ(layoutProblem(Library, Record), "");
  Record.Union = true;
  EXPECT_EQ(layoutProblem(Library, Record), "B does not begin its union");
}

TEST(LocalTypesNesting, AMemberInAUnionsPaddingBelongsToIt) {
  // union { ULONGLONG Quad; struct { ULONG Overlay[3]; ULONG Pad; }; }, as
  // _KUSER_SHARED_DATA has TickCountPad: the union pads Overlay to 16.
  std::vector<FlatMember> Flat = {flat("Quad", Builtin::ULongLong, 0),
                                  array("Overlay", Builtin::ULong, 3, 0),
                                  flat("Pad", Builtin::ULong, 12),
                                  flat("Cookie", Builtin::ULong, 16)};
  const RecordDef Record = nested(Flat);
  EXPECT_EQ(shapeOf(Record),
            "struct{union@0{Quad@0 struct@0{Overlay@0 Pad@12}} Cookie@16}");
  EXPECT_EQ(Record.Members[0].Type->Record->Size, 16u);
  expectRoundTrip(Record, Flat);

  // Packed to four, the union ends with Overlay.
  Flat[0].Alignment = 4;
  const RecordDef Packed = nested(Flat);
  EXPECT_EQ(shapeOf(Packed),
            "struct{union@0{Quad@0 Overlay@0} Pad@12 Cookie@16}");
  EXPECT_EQ(Packed.Members[0].Type->Record->Size, 12u);
  expectRoundTrip(Packed, Flat);
}
