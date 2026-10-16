//===- TypeRenderTests.cpp - Local types as C, C++, Rust and Go -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/types/PDBTypeImport.h"
#include "neverd/types/TypeParse.h"
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

TEST_F(TypeRenderFixture, RustTakesWindowsNamesAsAliases) {
  // Exact widths unless asked, as windows-rs spells them.
  EXPECT_TRUE(contains(render("_THREAD_LIKE", in(Language::Rust)),
                       "    pub MiscFlags: i32,"));
  RenderOptions Options = in(Language::Rust);
  Options.Spelling = BuiltinSpelling::Windows;
  const std::string Text = header("_THREAD_LIKE", Options);
  // The names the types use, declared as wdk-sys declares them, at the
  // program's widths.
  EXPECT_TRUE(contains(Text, "\n/// The Windows headers' names of builtin "
                             "types.\npub type UCHAR = u8;\npub type LONG = "
                             "i32;\npub type ULONG = u32;\npub type "
                             "ULONGLONG = u64;\n"))
      << Text;
  EXPECT_TRUE(contains(Text, "    pub Header: ULONGLONG,"));
  EXPECT_TRUE(contains(Text, "    pub MiscFlags: LONG,"));
  EXPECT_TRUE(contains(Text, "    pub Tail: [UCHAR; 3],"));
  // A bitfield reads as its own type; its storage stays a width.
  EXPECT_TRUE(contains(Text, "    pub _bitfield: u32,"));
  EXPECT_TRUE(
      contains(Text, "    pub const fn ReadyTransition(&self) -> ULONG {"));
  // A spelling Rust does not take renders as its default.
  Options.Spelling = BuiltinSpelling::IDA;
  EXPECT_TRUE(
      contains(header("_THREAD_LIKE", Options), "    pub MiscFlags: i32,"));
  EXPECT_EQ(spellingOf(Options), BuiltinSpelling::Portable);
  EXPECT_EQ(spellingsText(Language::Rust), "windows or portable");
  EXPECT_EQ(spellingsText(Language::Go), "windows or portable");
  EXPECT_EQ(spellingsText(Language::C), "windows, ida, keywords or portable");
}

TEST_F(TypeRenderFixture, GoTakesWindowsNamesAsAliases) {
  // Exact widths unless asked, as golang.org/x/sys/windows spells them.
  EXPECT_TRUE(
      contains(render("_THREAD_LIKE", in(Language::Go)), "\tHeader uint64"));
  RenderOptions Options = in(Language::Go);
  Options.Spelling = BuiltinSpelling::Windows;
  const std::string Text = header("_THREAD_LIKE", Options);
  EXPECT_TRUE(contains(Text, "\n// The Windows headers' names of builtin "
                             "types.\ntype UCHAR = uint8\ntype LONG = "
                             "int32\ntype ULONG = uint32\ntype ULONGLONG = "
                             "uint64\n"))
      << Text;
  EXPECT_TRUE(contains(Text, "\tHeader ULONGLONG"));
  EXPECT_TRUE(contains(Text, "\tTail [3]UCHAR"));
  EXPECT_TRUE(contains(Text, ") MiscFlags() *LONG {"));
  // A bitfield reads as its own type; its storage stays a width.
  EXPECT_TRUE(contains(Text, "\t_bitfield uint32"));
  EXPECT_TRUE(contains(Text, ") ReadyTransition() ULONG {"));
  EXPECT_TRUE(contains(Text, ") SetReadyTransition(value ULONG) {"));
}

TEST_F(TypeRenderFixture, PythonDeclaresRecordsAsCtypesDoes) {
  RenderOptions Options = in(Language::Python);
  Options.Asserts = true;
  const std::string Text = header("_THREAD_LIKE", Options);
  // Declared first and laid out after, so that a pointer may name a record
  // laid out later.
  EXPECT_TRUE(contains(Text, "class _THREAD_LIKE(ctypes.Structure):\n    "
                             "\"\"\"_THREAD_LIKE: 0x18 (24) bytes.\"\"\"\n"))
      << Text;
  EXPECT_LT(Text.find("class _THREAD_LIKE("),
            Text.find("_THREAD_LIKE._fields_ = ["));
  // Records in place are classes of their own, which _anonymous_ promotes.
  EXPECT_TRUE(contains(Text, "class _THREAD_LIKE_0(ctypes.Union):"));
  EXPECT_TRUE(contains(
      Text, "_THREAD_LIKE._anonymous_ = (\"Anonymous1\", \"Anonymous2\")\n"));
  // A run of bitfields is storage read through properties, each run's
  // storage named apart from the others the record promotes.
  EXPECT_TRUE(contains(Text, "(\"_bitfield\", ctypes.c_uint32),"));
  EXPECT_TRUE(contains(Text, "(\"_bitfield1\", ctypes.c_uint32),"));
  EXPECT_TRUE(
      contains(Text, "    @property\n    def ReadyTransition(self) -> int:\n"));
  EXPECT_TRUE(contains(Text, "        return (self._bitfield >> 1) & 0x1\n"));
  EXPECT_TRUE(contains(Text, "        return (self._bitfield1 >> 2) & 0x1\n"));
  EXPECT_TRUE(contains(
      Text, "assert ctypes.sizeof(_THREAD_LIKE) == 0x18, \"_THREAD_LIKE\"\n"));
  EXPECT_TRUE(contains(Text, "assert _THREAD_LIKE.Tail.offset == 0x10, "
                             "\"_THREAD_LIKE.Tail\"\n"));

  // A pack lays out as MSVC does, which Python 3.14 asks for by name.
  EXPECT_TRUE(contains(header("_PACKED", in(Language::Python)),
                       "_PACKED._pack_ = 1\n_PACKED._layout_ = \"ms\"\n"
                       "_PACKED._fields_ = [\n"));
  // Alone, a record's layout is in its class.
  EXPECT_TRUE(
      contains(render("_PACKED", in(Language::Python)),
               "class _PACKED(ctypes.Structure):\n    \"\"\"_PACKED: "
               "0x7 (7) bytes, packed to 1.\"\"\"\n\n    _pack_ = 1\n"));

  // An enumeration is an IntEnum, its field the underlying integer; a
  // function's address is ctypes.c_void_p, its C type in the comment.
  const std::string Device = header("_DEVICE", in(Language::Python));
  EXPECT_TRUE(contains(Device, "class _POOL_KIND(enum.IntEnum):"));
  EXPECT_TRUE(contains(Device, "(\"Pool\", ctypes.c_int32),"));
  EXPECT_TRUE(contains(Device, "enum _POOL_KIND\n"));
  EXPECT_TRUE(contains(Device, "(\"MajorFunction\", ctypes.c_void_p * 4),"));
  EXPECT_TRUE(contains(Device, "(*[4])(struct _DEVICE *, "));

  // Windows' names are aliases of exact widths at the top.
  Options = in(Language::Python);
  Options.Spelling = BuiltinSpelling::Windows;
  const std::string Windows = header("_PACKED", Options);
  EXPECT_TRUE(contains(Windows, "UCHAR = ctypes.c_uint8\n")) << Windows;
  EXPECT_TRUE(contains(Windows, "(\"Value\", ULONG),"));
}

TEST(TypeRender, PythonBindsTypedefsAndLaysOutInOrder) {
  // A typedef is bound before a layout names it; one of an array after the
  // layout of what it holds, since ctypes sizes an array type once.
  TypeLibrary Library;
  ASSERT_TRUE(parseCTypes(R"(
    typedef struct _NODE NODE;
    struct _NODE { NODE *Next; unsigned long Value; };
    typedef struct _PAIR { unsigned char A; unsigned long B; } PAIR;
    typedef PAIR PAIRS[2];
    struct _HOLDER { PAIRS Items; NODE Head; };
  )",
                          Library)
                  .ok());
  const std::string Text = renderTypes(Library, {}, in(Language::Python));
  const auto at = [&](llvm::StringRef Part) {
    const size_t Found = Text.find(Part.str());
    EXPECT_NE(Found, std::string::npos) << Part.str() << "\n" << Text;
    return Found;
  };
  EXPECT_LT(at("NODE = _NODE\n"), at("_NODE._fields_ = ["));
  EXPECT_LT(at("_PAIR._fields_ = ["), at("PAIRS = PAIR * 2\n"));
  EXPECT_LT(at("PAIRS = PAIR * 2\n"), at("_HOLDER._fields_ = ["));
  EXPECT_LT(at("_NODE._fields_ = ["), at("_HOLDER._fields_ = ["));
  EXPECT_TRUE(contains(Text, "(\"Next\", ctypes.POINTER(NODE)),"));
}

TEST_F(TypeRenderFixture, JavaScriptReadsRecordsThroughDataView) {
  RenderOptions Options = in(Language::JavaScript);
  Options.Asserts = true;
  const std::string Thread = header("_THREAD_LIKE", Options);
  // TypeScript checks the module by its JSDoc.
  EXPECT_TRUE(Thread.starts_with("// @ts-check\n")) << Thread;
  EXPECT_TRUE(contains(Thread, "export class _THREAD_LIKE {\n"));
  EXPECT_TRUE(contains(Thread, "  static size = 0x18;\n"));
  // The caller gives the bytes, where the record begins and their order.
  EXPECT_TRUE(contains(
      Thread, "  constructor(view, offset = 0, littleEndian = true) {"));
  // An anonymous record's members are the record's own, as C reaches them;
  // a bitfield reads its unit in 32-bit arithmetic.
  EXPECT_TRUE(contains(Thread, "  get ReadyTransition() { return "
                               "(this.#view.getUint32(this.#offset + 0x08, "
                               "this.#little) >>> 1) & 0x1; }\n"));
  EXPECT_TRUE(contains(Thread, "(unit & 0xFFFFFFFD) | ((value << 1) & 0x2)"));
  EXPECT_FALSE(contains(Thread, "class _THREAD_LIKE_0"));
  // An array's elements by index, which the module checks.
  EXPECT_TRUE(contains(Thread, "  Tail(i) { return this.#view.getUint8("
                               "this.#offset + 0x10 + 1 * index(i, 3)); }\n"));
  EXPECT_TRUE(contains(Thread, "function index(i, count) {"));
  // Each accessor read and written back within a buffer of the record's
  // size, which throws past its end.
  EXPECT_TRUE(contains(Thread, "check(_THREAD_LIKE, (r) => {\n"
                               "  r.Header = r.Header;\n"));
  EXPECT_TRUE(contains(Thread, "  r.setTail(2, r.Tail(2));\n"));

  const std::string Device = header("_DEVICE", Options);
  // A record held in place is a view of its bytes; an eight-byte value,
  // a pointer's address among them, is a bigint.
  EXPECT_TRUE(contains(Device, "  get Links() { return new _LIST_ENTRY("
                               "this.#view, this.#offset + 0x08, "
                               "this.#little); }\n"));
  EXPECT_TRUE(contains(Device, "[0x08, _LIST_ENTRY.size]"));
  EXPECT_TRUE(contains(Device, "  MajorFunction(i) { return "
                               "this.#view.getBigUint64(this.#offset + 0x18 + "
                               "8 * index(i, 4), this.#little); }\n"));
  // An enumeration is a frozen object, its field the underlying integer.
  EXPECT_TRUE(contains(Device, "export const _POOL_KIND = Object.freeze({\n"));
  EXPECT_TRUE(contains(Device, "  MustSucceed: -2147483648,\n"));
  EXPECT_TRUE(contains(Device, "/** @typedef {number} _POOL_KIND */\n"));
  EXPECT_TRUE(contains(Device, "/** 0x50 enum _POOL_KIND Pool @returns "
                               "{_POOL_KIND} */\n  get Pool() { return "
                               "this.#view.getInt32("));

  // A bitfield of an eight-byte unit reads as a bigint.
  EXPECT_TRUE(contains(header("_PUSH_LOCK", in(Language::JavaScript)),
                       "  get Shared() { return BigInt.asUintN(60, "
                       "this.#view.getBigUint64(this.#offset + 0x0, "
                       "this.#little) >> 4n); }\n"));

  // Windows' names are typedefs of the JavaScript types.
  Options = in(Language::JavaScript);
  Options.Spelling = BuiltinSpelling::Windows;
  const std::string Windows = header("_PACKED", Options);
  EXPECT_TRUE(contains(Windows, "/** @typedef {number} ULONG */\n")) << Windows;
  EXPECT_TRUE(contains(Windows, "@returns {ULONG} */"));
}

TEST(TypeRender, JavaScriptExtendsSignsAndNamesWhatItDeclares) {
  TypeLibrary Library;
  ASSERT_TRUE(parseCTypes(R"(
    typedef struct _NODE NODE, *PNODE;
    struct _NODE { PNODE Next; int Low : 3; int High : 29; };
    struct DataView { int constructor; __int128 Wide; };
  )",
                          Library)
                  .ok());
  const std::string Text = renderTypes(Library, {}, in(Language::JavaScript));
  // A signed field's top bit is the 32nd and back.
  EXPECT_TRUE(contains(Text, "  get Low() { return (this.#view.getUint32("
                             "this.#offset + 0x08, this.#little) << 29) >> "
                             "29; }\n"))
      << Text;
  EXPECT_TRUE(contains(Text, "  get High() { return (this.#view.getUint32("
                             "this.#offset + 0x08, this.#little) << 0) >> "
                             "3; }\n"));
  // A typedef of a record names its class; one of a pointer, its address.
  EXPECT_TRUE(contains(Text, "/** @typedef {_NODE} NODE */\n"));
  EXPECT_TRUE(contains(Text, "export const NODE = _NODE;\n"));
  EXPECT_TRUE(contains(Text, "/** @typedef {bigint} PNODE The address of "));
  EXPECT_TRUE(contains(Text, "@returns {PNODE} */\n  get Next() {"));
  // What the module itself names, and a class's own words, step aside.
  EXPECT_TRUE(contains(Text, "export class DataView_ {"));
  EXPECT_TRUE(contains(Text, "  get constructor_() {"));
  // Sixteen bytes read as one bigint from two halves.
  EXPECT_TRUE(contains(Text, "  get Wide() { return this.#wide("));
  EXPECT_TRUE(contains(Text, "  #wide(at, signed) {"));
}

TEST_F(TypeRenderFixture, LuaReadsRecordsThroughStringUnpack) {
  RenderOptions Options = in(Language::Lua);
  Options.Asserts = true;
  const std::string Thread = header("_THREAD_LIKE", Options);
  // A module of record types, each in a block of its own.
  EXPECT_TRUE(Thread.starts_with("-- ")) << Thread;
  EXPECT_TRUE(contains(Thread, "local M = {}\n"));
  EXPECT_TRUE(contains(Thread, "do\n  local T = record(\"_THREAD_LIKE\", "
                               "0x18)\n  M._THREAD_LIKE = T\n"));
  EXPECT_TRUE(Thread.ends_with("return M\n"));
  // An anonymous record's members are the record's own; a bitfield masks
  // its unit, which string.pack takes whole.
  EXPECT_TRUE(contains(Thread, "  function T.get.ReadyTransition(r) return "
                               "(get(r, 0x08, \"I4\") >> 1) & 0x1 end\n"));
  EXPECT_TRUE(contains(Thread, "put(r, 0x08, \"I4\", (get(r, 0x08, \"I4\") & "
                               "0xFFFFFFFD) | ((value << 1) & 0x2))"));
  // An array's elements by index, through methods.
  EXPECT_TRUE(contains(Thread, "  function T.methods.Tail(r, i) return "
                               "get(r, 0x10 + 1 * index(i, 3), \"B\") end\n"));
  EXPECT_TRUE(contains(Thread, "  function T.methods.setTail(r, i, value)"));
  // Each accessor read and written back within bytes of the record's size.
  EXPECT_TRUE(contains(Thread, "check(M._THREAD_LIKE, function(r)\n"
                               "  r.Header = r.Header\n"));
  EXPECT_TRUE(contains(Thread, "  r:setTail(2, r:Tail(2))\n"));

  const std::string Device = header("_DEVICE", Options);
  // A record held in place is a view of the same bytes; a pointer reads
  // as its address.
  EXPECT_TRUE(contains(Device, "  function T.get.Links(r) return "
                               "M._LIST_ENTRY.view(r[BYTES], r[AT] + 0x08, "
                               "r[ORDER] == \">\") end\n"));
  EXPECT_TRUE(contains(Device, "{ 0x08, M._LIST_ENTRY.size }"));
  EXPECT_TRUE(contains(Device, "-- 0x38 struct _DEVICE *Next: address\n"));
  EXPECT_TRUE(contains(Device, "M._POOL_KIND = {\n"));
  EXPECT_TRUE(contains(Device, "  MustSucceed = -2147483648,\n"));
  EXPECT_TRUE(contains(Device, "  function T.get.Pool(r) return get(r, "
                               "0x50, \"i4\") end\n"));

  // A signed bitfield extends its top bit; an eight-byte unit is one of
  // Lua's integers.
  EXPECT_TRUE(contains(header("_PUSH_LOCK", in(Language::Lua)),
                       "  function T.get.Shared(r) return (get(r, 0x0, "
                       "\"I8\") >> 4) & 0xFFFFFFFFFFFFFFF end\n"));
}

TEST(TypeRender, LuaExtendsSignsAndNamesWhatItDeclares) {
  TypeLibrary Library;
  ASSERT_TRUE(parseCTypes(R"(
    typedef struct _NODE NODE, *PNODE;
    struct _NODE { PNODE Next; int Low : 3; int end; __int128 Wide; };
  )",
                          Library)
                  .ok());
  const std::string Text = renderTypes(Library, {}, in(Language::Lua));
  EXPECT_TRUE(contains(Text, "  function T.get.Low(r) local v = get(r, "
                             "0x08, \"I4\") & 0x7; return (v ~ 0x4) - 0x4 "
                             "end\n"))
      << Text;
  // A typedef of a record names its type; one of a pointer, an address.
  EXPECT_TRUE(contains(Text, "M.NODE = M._NODE\n"));
  EXPECT_TRUE(contains(Text, "-- PNODE is struct _NODE *, which reads as an "
                             "address.\n"));
  // Lua's words step aside; sixteen bytes read as the string they are.
  EXPECT_TRUE(contains(Text, "  function T.get.end_(r)"));
  EXPECT_TRUE(contains(Text, "  function T.get.Wide(r) return raw(r, 0x10, "
                             "16) end\n"));
}

TEST_F(TypeRenderFixture, EmbeddedCDefinesWhatAUnitNames) {
  // A decompiled unit holds the records it names, what they hold by value,
  // and declarations of what they only point to, each under a guard.
  const LocalType *Device = Library->find("_DEVICE");
  const LocalType *Uses = Library->find("_USES_FORWARD");
  ASSERT_TRUE(Device && Uses);
  RenderOptions Options = in(Language::C);
  Options.Spelling = BuiltinSpelling::Portable;
  Options.Asserts = true;
  const LocalType *Roots[] = {Device, Uses};
  const EmbeddedC Embedded = renderEmbeddedC(*Library, Roots, Options);
  const std::string &Text = Embedded.Text;
  EXPECT_FALSE(contains(Text, "#pragma once")) << Text;
  EXPECT_TRUE(contains(Text, "#ifndef NEVERD_TYPE__DEVICE\n"
                             "#define NEVERD_TYPE__DEVICE\n"));
  EXPECT_TRUE(contains(Text, "struct _DEVICE\n{\n"));
  EXPECT_TRUE(contains(Text, "static_assert(sizeof(struct _DEVICE) == 0x58"));
  // By value: defined before the record that holds them.
  EXPECT_LT(Text.find("union _LARGE_INTEGER\n{"),
            Text.find("struct _DEVICE\n{"));
  EXPECT_TRUE(contains(Text, "enum _POOL_KIND"));
  // Only pointed to: declared.
  EXPECT_TRUE(contains(Text, "struct _FORWARD_ONLY;\n"));
  EXPECT_FALSE(contains(Text, "struct _FORWARD_ONLY\n{"));
  EXPECT_TRUE(llvm::is_contained(
      Embedded.Records, std::pair<std::string, bool>("_DEVICE", false)));
  EXPECT_TRUE(llvm::is_contained(
      Embedded.Records, std::pair<std::string, bool>("_LARGE_INTEGER", true)));
  EXPECT_TRUE(llvm::is_contained(Embedded.Identifiers, "MustSucceed"));
  EXPECT_TRUE(llvm::is_contained(Embedded.Types, "_PUSH_LOCK"));
  EXPECT_FALSE(llvm::is_contained(Embedded.Types, "_FORWARD_ONLY"));
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
