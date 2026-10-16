//===- TypeRenderTests.cpp - Local types as C, C++, Rust and Go -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/types/PDBTypeImport.h"
#include "neverd/types/TypeRender.h"

#include <gtest/gtest.h>

using namespace neverd::types;

namespace {

std::unique_ptr<TypeLibrary> loadFixture() {
  auto Library = importPDBTypes(NEVERD_TYPES_FIXTURES "/local_types.pdb");
  if (!Library) {
    ADD_FAILURE() << llvm::toString(Library.takeError());
    return nullptr;
  }
  return std::move(*Library);
}

RenderOptions in(Language Lang) {
  RenderOptions Options;
  Options.Lang = Lang;
  return Options;
}

class TypeRenderFixture : public ::testing::Test {
protected:
  void SetUp() override {
    Library = loadFixture();
    ASSERT_NE(Library, nullptr);
  }

  std::string render(llvm::StringRef Name, RenderOptions Options) const {
    const LocalType *Type = Library->find(Name);
    EXPECT_NE(Type, nullptr) << Name.str();
    return Type ? renderType(*Library, *Type, Options) : std::string();
  }

  std::string header(llvm::StringRef Name, RenderOptions Options) const {
    const LocalType *Type = Library->find(Name);
    EXPECT_NE(Type, nullptr) << Name.str();
    if (!Type)
      return std::string();
    const LocalType *Roots[] = {Type};
    return renderTypes(*Library, Roots, Options);
  }

  std::unique_ptr<TypeLibrary> Library;
};

Member member(std::string Name, TypeExprRef Type, uint64_t Byte) {
  Member Field;
  Field.Name = std::move(Name);
  Field.Type = std::move(Type);
  Field.BitOffset = Byte * 8;
  return Field;
}

Member bitfield(std::string Name, Builtin Type, uint64_t Byte,
                uint32_t Position, uint32_t Width) {
  Member Field = member(std::move(Name), makeBuiltin(Type), Byte);
  Field.StorageBitOffset = Field.BitOffset;
  Field.BitOffset += Position;
  Field.BitWidth = Width;
  return Field;
}

LocalType structure(std::string Name, uint64_t Size,
                    std::vector<Member> Members) {
  LocalType Type;
  Type.Name = std::move(Name);
  Type.Kind = TypeKind::Struct;
  auto Record = std::make_shared<RecordDef>();
  Record->Size = Size;
  Record->Members = std::move(Members);
  Type.Record = std::move(Record);
  return Type;
}

bool contains(const std::string &Text, llvm::StringRef Part) {
  return llvm::StringRef(Text).contains(Part);
}

} // namespace

TEST(TypeRender, NamesItsLanguages) {
  for (Language Lang :
       {Language::C, Language::Cpp, Language::Rust, Language::Go})
    EXPECT_EQ(languageNamed(languageId(Lang)), Lang);
  EXPECT_EQ(languageNamed("RUST"), Language::Rust);
  EXPECT_EQ(languageNamed("pascal"), std::nullopt);
}

TEST_F(TypeRenderFixture, CShowsEveryOffsetAndBit) {
  EXPECT_EQ(render("_THREAD_LIKE", in(Language::C)),
            R"(// _THREAD_LIKE: 0x18 (24) bytes
struct _THREAD_LIKE
{
    ULONGLONG Header;                                           // 0x00
    union                                                       // 0x08
    {
        struct                                                  // 0x08
        {
            ULONG AutoBoostActive : 1;                          // 0x08 bit 0
            ULONG ReadyTransition : 1;                          // 0x08 bit 1
            ULONG MiscSpare : 30;                               // 0x08 bits 2..31
        };
        LONG MiscFlags;                                         // 0x08
    };
    union                                                       // 0x0C
    {
        struct                                                  // 0x0C
        {
            ULONG ThreadFlagsSpare : 2;                         // 0x0C bits 0..1
            ULONG AutoAlignment : 1;                            // 0x0C bit 2
            ULONG DisableBoost : 1;                             // 0x0C bit 3
            ULONG ThreadReserved : 28;                          // 0x0C bits 4..31
        };
        LONG ThreadFlags;                                       // 0x0C
    };
    UCHAR Tail[3];                                              // 0x10
    // 0x13: 5 bytes of padding
};
)");
}

TEST_F(TypeRenderFixture, CSpellsBuiltinsAsAsked) {
  RenderOptions Options = in(Language::C);
  Options.Spelling = BuiltinSpelling::IDA;
  EXPECT_TRUE(contains(render("_THREAD_LIKE", Options),
                       "unsigned int AutoBoostActive : 1;"));
  Options.Spelling = BuiltinSpelling::Portable;
  EXPECT_TRUE(contains(render("_THREAD_LIKE", Options), "uint64_t Header;"));
  Options.Spelling = BuiltinSpelling::Keywords;
  EXPECT_TRUE(
      contains(render("_THREAD_LIKE", Options), "unsigned long long Header;"));
  Options.HexOffsets = false;
  EXPECT_TRUE(contains(render("_THREAD_LIKE", Options), "// +16"));
}

TEST_F(TypeRenderFixture, AHeaderDefinesWhatATypeHoldsFirst) {
  RenderOptions Options = in(Language::C);
  Options.Asserts = true;
  const std::string Text = header("_DEVICE", Options);
  // Records it points to are declared; what it holds is defined before it.
  EXPECT_TRUE(contains(Text, "struct _DEVICE;\n"));
  const size_t Device = Text.find("struct _DEVICE\n{");
  for (const char *Held : {"struct _LIST_ENTRY\n{", "union _LARGE_INTEGER\n{",
                           "struct _PUSH_LOCK\n{", "enum _POOL_KIND\n{"}) {
    const size_t At = Text.find(Held);
    ASSERT_NE(At, std::string::npos) << Held;
    EXPECT_LT(At, Device) << Held;
  }
  EXPECT_FALSE(contains(Text, "_THREAD_LIKE"));
  EXPECT_TRUE(contains(Text, "typedef uint32_t ULONG;"));
  // The header compiles as C++ too: C names its check as C++ does.
  EXPECT_TRUE(contains(Text, "#if !defined(__cplusplus) && "
                             "!defined(static_assert)\n#define static_assert "
                             "_Static_assert\n#endif\n"));
  EXPECT_TRUE(
      contains(Text, "\nstatic_assert(offsetof(struct _DEVICE, Pool) == 0x50, "
                     "\"_DEVICE.Pool\");"));
}

TEST_F(TypeRenderFixture, CppNamesTypesBare) {
  RenderOptions Options = in(Language::Cpp);
  Options.Asserts = true;
  const std::string Text = header("_DEVICE", Options);
  EXPECT_TRUE(contains(Text, "struct _DEVICE;\n"));
  EXPECT_TRUE(contains(Text, "    _LIST_ENTRY Links;"));
  EXPECT_TRUE(contains(Text, "static_assert(sizeof(_DEVICE) == 0x58"));
}

TEST_F(TypeRenderFixture, CKeepsPacking) {
  const std::string Text = render("_PACKED", in(Language::C));
  EXPECT_TRUE(contains(Text, "#pragma pack(push, 1)\nstruct _PACKED\n"));
  EXPECT_TRUE(contains(Text, "    ULONG Value;"));
  EXPECT_TRUE(contains(Text, "};\n#pragma pack(pop)\n"));
}

TEST_F(TypeRenderFixture, RustNamesAnonymousRecordsAsWindowsRsDoes) {
  const std::string Text = render("_THREAD_LIKE", in(Language::Rust));
  EXPECT_TRUE(contains(Text, "#[repr(C)]\n#[derive(Clone, Copy)]\npub struct "
                             "_THREAD_LIKE {\n"));
  EXPECT_TRUE(contains(Text, "    pub Anonymous1: _THREAD_LIKE_0,"));
  EXPECT_TRUE(contains(Text, "    pub Anonymous2: _THREAD_LIKE_1,"));
  EXPECT_TRUE(contains(Text, "pub union _THREAD_LIKE_0 {\n"));
  EXPECT_TRUE(contains(Text, "    pub MiscFlags: i32,"));
  EXPECT_TRUE(contains(Text, "    pub _bitfield: u32,"));
  EXPECT_TRUE(contains(Text, "    pub const fn ReadyTransition(&self) -> u32 "
                             "{\n        (self._bitfield >> 1) & 0x1\n    }"));
}

TEST_F(TypeRenderFixture, RustAndGoKeepPacking) {
  EXPECT_TRUE(
      contains(render("_PACKED", in(Language::Rust)), "#[repr(C, packed(1))]"));
  // Go cannot align a member below its alignment: bytes and an accessor.
  const std::string Go = render("_PACKED", in(Language::Go));
  EXPECT_TRUE(contains(Go, "\tValue_ [4]byte"));
  EXPECT_TRUE(contains(Go, "func (s *_PACKED) Value() *uint32 {\n\treturn "
                           "(*uint32)(unsafe.Pointer(&s.Value_))\n}"));
}

TEST_F(TypeRenderFixture, GoReadsAUnionThroughAccessors) {
  const std::string Text = render("_LARGE_INTEGER", in(Language::Go));
  EXPECT_TRUE(contains(Text, "type _LARGE_INTEGER struct {\n\traw [1]uint64"));
  EXPECT_TRUE(contains(Text, "func (s *_LARGE_INTEGER) QuadPart() *int64 {"));
  EXPECT_TRUE(contains(Text, "func (s *_LARGE_INTEGER) u() *_LARGE_INTEGER_1"));
}

TEST_F(TypeRenderFixture, EnumerationsKeepTheirValues) {
  EXPECT_TRUE(contains(render("_SIGNED_KIND", in(Language::C)),
                       "    Negative = -1,\n"));
  EXPECT_TRUE(contains(render("_SIGNED_KIND", in(Language::Rust)),
                       "pub type _SIGNED_KIND = i32;\npub const Negative: "
                       "_SIGNED_KIND = -1;"));
  EXPECT_TRUE(contains(render("_SIGNED_KIND", in(Language::Go)),
                       "\tNegative _SIGNED_KIND = -1\n"));
}

TEST(TypeRender, SignedBitfieldsReadTheirSign) {
  TypeLibrary Library;
  Library.define(structure("_FLAGS", 4,
                           {bitfield("Low", Builtin::Long, 0, 0, 4),
                            bitfield("Delta", Builtin::Long, 0, 4, 4)}));
  const LocalType &Flags = *Library.find("_FLAGS");
  // C extends a signed field's top bit, so the accessors do too.
  EXPECT_TRUE(contains(renderType(Library, Flags, in(Language::Rust)),
                       "pub const fn Delta(&self) -> i32 {\n        "
                       "((self._bitfield << 24) as i32) >> 28\n    }"));
  EXPECT_TRUE(contains(renderType(Library, Flags, in(Language::Go)),
                       "func (s *_FLAGS) Delta() int32 {\n\treturn "
                       "int32(s._bitfield<<24) >> 28\n}"));
}

TEST(TypeRender, UnnamedBitsAndClosedRunsStay) {
  // Bits 0..3 of the first unit are unnamed, and the second unit begins
  // although the first has room.
  TypeLibrary Library;
  Library.define(structure("_BITS", 8,
                           {bitfield("A", Builtin::ULong, 0, 4, 4),
                            bitfield("B", Builtin::ULong, 4, 0, 4)}));
  const std::string Text =
      renderType(Library, *Library.find("_BITS"), in(Language::C));
  EXPECT_TRUE(contains(Text, "    ULONG : 4;"));
  EXPECT_TRUE(contains(Text, "    ULONG A : 4;"));
  EXPECT_TRUE(contains(Text, "    ULONG : 0;\n    ULONG B : 4;"));
}

TEST(TypeRender, AMemberPastItsPlaceTakesAFiller) {
  // MSVC put Wide at 0x10, as __declspec(align(16)) would.
  TypeLibrary Library;
  Library.define(
      structure("_ALIGNED", 0x20,
                {member("Head", makeBuiltin(Builtin::ULong), 0),
                 member("Wide", makeBuiltin(Builtin::ULongLong), 0x10)}));
  const LocalType &Aligned = *Library.find("_ALIGNED");
  EXPECT_TRUE(contains(renderType(Library, Aligned, in(Language::C)),
                       "UCHAR __filler0[12];"));
  EXPECT_TRUE(contains(renderType(Library, Aligned, in(Language::Rust)),
                       "pub _filler1: [u8; 12],"));
  EXPECT_TRUE(
      contains(renderType(Library, Aligned, in(Language::Go)), "\t_ [12]byte"));
}

TEST(TypeRender, GoReadsAFlexibleArrayThroughAnAccessor) {
  TypeLibrary Library;
  Library.define(structure(
      "_LIST", 8,
      {member("Count", makeBuiltin(Builtin::ULong), 0),
       member("Items", makeArray(makeBuiltin(Builtin::UShort), 0), 4)}));
  const std::string Text =
      renderType(Library, *Library.find("_LIST"), in(Language::Go));
  EXPECT_FALSE(contains(Text, "\tItems [0]uint16"));
  EXPECT_TRUE(contains(Text, "func (s *_LIST) Items(n int) []uint16 {"));
}

TEST(TypeRender, CCharacterTypesLeaveCppItsKeywords) {
  // C declares the character types C++ has keywords for, so that its header
  // compiles as either; char8_t is a keyword from C++20.
  TypeLibrary Library;
  Library.define(structure("_TEXT", 4,
                           {member("Narrow", makeBuiltin(Builtin::Char8), 0),
                            member("Wide", makeBuiltin(Builtin::Char16), 2)}));
  for (BuiltinSpelling Spelling :
       {BuiltinSpelling::Windows, BuiltinSpelling::Keywords}) {
    RenderOptions Options = in(Language::C);
    Options.Spelling = Spelling;
    const std::string Text = renderTypes(Library, {}, Options);
    EXPECT_TRUE(contains(Text, "#if !defined(__cplusplus) || "
                               "!defined(__cpp_char8_t)\ntypedef uint8_t "
                               "char8_t;\n#endif\n"))
        << Text;
    EXPECT_TRUE(contains(Text, "#ifndef __cplusplus\ntypedef uint16_t "
                               "char16_t;\n#endif\n"))
        << Text;
  }
  EXPECT_FALSE(contains(renderTypes(Library, {}, in(Language::Cpp)),
                        "typedef uint16_t char16_t;"));
}

TEST(TypeRender, NamesStepAroundKeywords) {
  TypeLibrary Library;
  Library.define(structure("_WORDS", 8,
                           {member("type", makeBuiltin(Builtin::ULong), 0),
                            member("class", makeBuiltin(Builtin::ULong), 4)}));
  const LocalType &Words = *Library.find("_WORDS");
  const std::string C = renderType(Library, Words, in(Language::C));
  EXPECT_TRUE(contains(C, "ULONG type;"));
  EXPECT_TRUE(contains(C, "ULONG class;"));
  EXPECT_TRUE(
      contains(renderType(Library, Words, in(Language::Cpp)), "ULONG class_;"));
  EXPECT_TRUE(contains(renderType(Library, Words, in(Language::Rust)),
                       "pub r#type: u32,"));
  EXPECT_TRUE(
      contains(renderType(Library, Words, in(Language::Go)), "\ttype_ uint32"));
}

TEST(TypeRender, NamesThatAreNoIdentifiersCompile) {
  TypeLibrary Library;
  Library.define(structure("Outer::Inner<int>", 4,
                           {member("x", makeBuiltin(Builtin::Int), 0)}));
  Library.define(structure(
      "Holder", 4,
      {member("Inner", makeNamed("Outer::Inner<int>", TypeKind::Struct), 0)}));
  for (const char *Name : {"_FIRST", "_SECOND"}) {
    LocalType Kind;
    Kind.Name = Name;
    Kind.Kind = TypeKind::Enum;
    auto Enum = std::make_shared<EnumDef>();
    Enum->Values = {{"None", 0, ""}, {std::string(Name) + "_One", 1, ""}};
    Kind.Enum = std::move(Enum);
    Library.define(std::move(Kind));
  }
  Library.define(
      structure("u32", 4, {member("x", makeBuiltin(Builtin::Int), 0)}));

  const std::string C =
      renderType(Library, *Library.find("Holder"), in(Language::C));
  EXPECT_TRUE(contains(C, "struct Outer__Inner_int_ Inner;"));
  // The comment keeps the name as the program spells it.
  EXPECT_TRUE(contains(
      renderType(Library, *Library.find("Outer::Inner<int>"), in(Language::C)),
      "// Outer::Inner<int>: 0x4 (4) bytes\nstruct "
      "Outer__Inner_int_\n"));
  // The second enumeration's None takes its name as a prefix.
  EXPECT_TRUE(
      contains(renderType(Library, *Library.find("_SECOND"), in(Language::C)),
               "    _SECOND_None = 0,\n"));
  EXPECT_TRUE(
      contains(renderType(Library, *Library.find("u32"), in(Language::Rust)),
               "pub struct u32_ {"));
  const auto Renamed = renamedNames(Library, Language::C);
  ASSERT_EQ(Renamed.size(), 2u);
  EXPECT_EQ(Renamed[0].first, "Outer::Inner<int>");
  EXPECT_EQ(Renamed[0].second, "Outer__Inner_int_");
  EXPECT_EQ(Renamed[1].first, "_SECOND::None");
  EXPECT_EQ(Renamed[1].second, "_SECOND_None");
}
