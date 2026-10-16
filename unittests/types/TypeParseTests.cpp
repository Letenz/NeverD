//===- TypeParseTests.cpp - Local types from C and C++ source -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "TypeShape.h"

#include "neverd/types/PDBTypeImport.h"
#include "neverd/types/TypeParse.h"
#include "neverd/types/TypeRender.h"

#include "llvm/ADT/StringExtras.h"

#include <cstdlib>
#include <gtest/gtest.h>

using namespace neverd::types;
using neverd::types::test::shapeOf;

namespace {

/// \p Source read into a new library, every problem a failure.
TypeLibrary parsed(llvm::StringRef Source, ParseOptions Options = {}) {
  TypeLibrary Library;
  const ParseResult Result = parseTypes(Source, Library, Options);
  for (const SourceProblem &Problem : Result.Problems)
    ADD_FAILURE() << Problem.Line << ":" << Problem.Column << ": "
                  << Problem.Message;
  return Library;
}

const RecordDef &recordOf(const TypeLibrary &Library, llvm::StringRef Name) {
  static const RecordDef Empty;
  const LocalType *Type = Library.find(Name);
  EXPECT_NE(Type, nullptr) << Name.str();
  return Type && Type->Record ? *Type->Record : Empty;
}

/// Every named member with its bit offset and width, anonymous records
/// looked through: what a layout is, whatever its nesting.
std::string leaves(const RecordDef &Record, uint64_t Base = 0) {
  std::string Out;
  for (const Member &Field : Record.Members) {
    if (Field.isAnonymous()) {
      Out += leaves(*Field.Type->Record, Base + Field.BitOffset);
      continue;
    }
    Out += Field.Name + "@" + std::to_string(Base + Field.BitOffset);
    if (Field.isBitfield())
      Out += ":" + std::to_string(Field.BitWidth);
    Out += " ";
  }
  return Out;
}

} // namespace

TEST(TypeParse, LaysAStructureOutAsMSVC) {
  const TypeLibrary Library = parsed(R"(
    struct A { char c; int i; short s; };
    struct B { unsigned long a : 4; unsigned long b : 4; unsigned short c : 3;
               unsigned long d : 1; };
  )");
  const RecordDef &A = recordOf(Library, "A");
  EXPECT_EQ(shapeOf(A), "struct{c@0 i@4 s@8}");
  EXPECT_EQ(A.Size, 12u);
  // A bitfield of another size begins a unit of its own.
  const RecordDef &B = recordOf(Library, "B");
  EXPECT_EQ(shapeOf(B), "struct{a@0b:4 b@4b:4 c@32b:3 d@64b:1}");
  EXPECT_EQ(B.Size, 12u);
}

TEST(TypeParse, BitfieldsShareUnitsUnderItanium) {
  ParseOptions Options;
  Options.Rules = LayoutRules::Itanium;
  const TypeLibrary Library = parsed(
      "struct B { unsigned a : 4; unsigned short c : 3; unsigned d : 30; };",
      Options);
  const RecordDef &B = recordOf(Library, "B");
  // c fits after a; d would cross a 32-bit unit and takes the next.
  EXPECT_EQ(shapeOf(B), "struct{a@0b:4 c@4b:3 d@32b:30}");
  EXPECT_EQ(B.Size, 8u);
}

TEST(TypeParse, ReadsAnonymousRecordsAndWindowsNames) {
  const TypeLibrary Library = parsed(R"(
    struct _THREAD_LIKE
    {
        ULONGLONG Header;                       // 0x00
        union                                   // 0x08
        {
            struct
            {
                ULONG AutoBoostActive : 1;      // 0x08 bit 0
                ULONG MiscSpare : 31;           // 0x08 bits 1..31
            };
            LONG MiscFlags;                     // 0x08
        };
        UCHAR Tail[3];                          // 0x0C
    };
  )");
  const RecordDef &Thread = recordOf(Library, "_THREAD_LIKE");
  EXPECT_EQ(shapeOf(Thread),
            "struct{Header@0 union@8{struct@0{AutoBoostActive@0b:1 "
            "MiscSpare@1b:31} MiscFlags@0} Tail@12}");
  EXPECT_EQ(Thread.Size, 16u);
}

TEST(TypeParse, KeepsPackingAndDeclaredAlignment) {
  const TypeLibrary Library = parsed(R"(
    #pragma pack(push, 1)
    struct P { UCHAR a; ULONG b; USHORT c; };
    #pragma pack(pop)
    struct DECLSPEC_ALIGN(16) Q { ULONGLONG Next; };
    struct R { UCHAR a; struct Q q; };
  )");
  const RecordDef &P = recordOf(Library, "P");
  EXPECT_EQ(shapeOf(P), "struct{a@0 b@1 c@5}");
  EXPECT_EQ(P.Size, 7u);
  EXPECT_EQ(P.Pack, 1u);
  EXPECT_EQ(recordOf(Library, "Q").Size, 16u);
  EXPECT_EQ(recordOf(Library, "Q").Alignment, 16u);
  EXPECT_EQ(shapeOf(recordOf(Library, "R")), "struct{a@0 q@16}");
}

TEST(TypeParse, KeepsADeclaredAlignmentUnderPack) {
  // Microsoft's compiler lowers a type's declared alignment for no pack, as
  // clang's MSVC layout shows: x at 16 in a record of 32 bytes.
  const TypeLibrary Library = parsed(R"(
    typedef struct __declspec(align(16)) _M128A {
      ULONGLONG Low; LONGLONG High;
    } M128A;
    #pragma pack(push, 4)
    struct P4 { CHAR c; M128A x; };
    struct P4m { CHAR c; __declspec(align(8)) LONGLONG y; };
    struct P4n { CHAR c; ULONGLONG z; };
    #pragma pack(pop)
  )");
  EXPECT_EQ(shapeOf(recordOf(Library, "P4")), "struct{c@0 x@16}");
  EXPECT_EQ(recordOf(Library, "P4").Size, 32u);
  EXPECT_EQ(Library.alignOf(*makeNamed("P4", TypeKind::Struct)), 16u);
  EXPECT_EQ(shapeOf(recordOf(Library, "P4m")), "struct{c@0 y@8}");
  // A natural alignment the pack lowers.
  EXPECT_EQ(shapeOf(recordOf(Library, "P4n")), "struct{c@0 z@4}");
  EXPECT_EQ(recordOf(Library, "P4n").Size, 12u);
}

TEST(TypeParse, ReadsEnumerationsWithConstants) {
  const TypeLibrary Library = parsed(R"(
    enum E { A, B = 5, C, D = B << 2, F = -1, G = (int)0x80000000, H = C | 1 };
    enum Small : unsigned char { Low = 1, High = 0xFF };
  )");
  const LocalType *E = Library.find("E");
  ASSERT_TRUE(E && E->Enum);
  std::string Values;
  for (const Enumerator &Value : E->Enum->Values)
    Values += Value.Name + "=" + std::to_string(int64_t(Value.Value)) + " ";
  EXPECT_EQ(Values, "A=0 B=5 C=6 D=20 F=-1 G=-2147483648 H=7 ");
  EXPECT_EQ(Library.find("Small")->Enum->Underlying, Builtin::UChar);
}

TEST(TypeParse, ReadsTypedefsPrototypesAndFunctionPointers) {
  const TypeLibrary Library = parsed(R"(
    typedef struct _DEVICE { LONG (__stdcall *Dispatch)(struct _DEVICE *Self,
                                                         VOID *Irp);
                             struct _DEVICE *Next; } DEVICE, *PDEVICE;
    typedef VOID (NTAPI *PROUTINE)(PDEVICE Device);
    LONG NTAPI IoCallDevice(PDEVICE Device, ULONG Length, ...);
  )");
  EXPECT_EQ(Library.find("DEVICE")->Kind, TypeKind::Typedef);
  EXPECT_EQ(Library.find("PDEVICE")->Target->Kind, TypeExprKind::Pointer);
  const RecordDef &Device = recordOf(Library, "_DEVICE");
  EXPECT_EQ(shapeOf(Device), "struct{Dispatch@0 Next@8}");
  const TypeExpr &Dispatch = *Device.Members[0].Type;
  ASSERT_EQ(Dispatch.Kind, TypeExprKind::Pointer);
  ASSERT_EQ(Dispatch.Element->Kind, TypeExprKind::Function);
  // x64 compilers accept __stdcall and ignore it: no convention is kept.
  EXPECT_EQ(Dispatch.Element->Function->Convention,
            CallingConvention::Unspecified);
  EXPECT_EQ(Dispatch.Element->Function->Params[0].Name, "Self");

  const FunctionDef *Call = Library.findPrototype("IoCallDevice");
  ASSERT_NE(Call, nullptr);
  EXPECT_EQ(Call->Convention, CallingConvention::Unspecified);
  ASSERT_EQ(Call->Params.size(), 2u);
  EXPECT_EQ(Call->Params[1].Name, "Length");
  EXPECT_TRUE(Call->Variadic);
  EXPECT_EQ(Library.find("PROUTINE")->Target->Element->Kind,
            TypeExprKind::Function);

  // 32-bit x86 keeps it.
  TypeLibrary X86(DataModel::ILP32);
  ASSERT_TRUE(parseCTypes("LONG NTAPI IoCallDevice(ULONG Length);", X86).ok());
  EXPECT_EQ(X86.findPrototype("IoCallDevice")->Convention,
            CallingConvention::Stdcall);
}

TEST(TypeParse, ReadsWhatGnuPreprocessedHeadersHold) {
  // As clang -E writes <ntifs.h> for mingw-w64: attributes wherever GNU
  // puts them, __extension__, inline definitions, assembler names, and a
  // #pragma pack that names a macro.
  const TypeLibrary Library = parsed(R"(
    #define PACKING 4
    typedef struct __attribute__ ((__aligned__ (16))) _SLIST {
      unsigned long long A, B;
    } SLIST, *PSLIST;
    #pragma pack(push, PACKING)
    typedef struct _P { char c; long long q; } P;
    #pragma pack(pop)
    typedef struct _M {
      unsigned long a;
      unsigned long b __attribute__((aligned(8)));
      unsigned char __attribute__ ((__aligned__ (8))) c;
    } M;
    typedef struct { int a; } X, *PX;
    typedef enum { A1, A2 = 5 } E, *PE;
    typedef struct _Q { char c; int i; } __attribute__((__packed__, __may_alias__)) Q;
    __extension__ typedef unsigned long long U64;
    typedef union _R {
      unsigned long n;
      __extension__ union { unsigned long w[0]; unsigned long v[0]; };
    } R;
    typedef struct _S { unsigned long n; union { unsigned long w[0]; }; } S;
    extern __inline__ __attribute__((__always_inline__,__gnu_inline__))
    int helper(int x) { return x + 1.5f > 0 ? 1 : 0; }
    void __attribute__((__cdecl__)) quit(int code) __attribute__ ((__noreturn__));
    int renamed(void) __asm__("_renamed");
    void *__attribute__((__cdecl__)) info(void);
    void logv(const char *format, va_list arguments);
    _Static_assert(sizeof(P) == 12, "packed to the macro");
    _Static_assert(__alignof__(SLIST) == 16, "aligned record");
    _Static_assert(__builtin_offsetof(M, b) == 8, "aligned member");
    _Static_assert(sizeof(int) == 4 ? 1 : 0, "a conditional");
  )");
  EXPECT_EQ(recordOf(Library, "_SLIST").Size, 16u);
  EXPECT_EQ(Library.alignOf(*makeNamed("_SLIST", TypeKind::Struct)), 16u);
  EXPECT_EQ(shapeOf(recordOf(Library, "_M")), "struct{a@0 b@8 c@16}");
  EXPECT_EQ(recordOf(Library, "_M").Size, 24u);
  // An anonymous record's typedef names it; the others point to it.
  EXPECT_EQ(Library.find("X")->Kind, TypeKind::Struct);
  EXPECT_EQ(Library.find("PX")->Target->Element->Name, "X");
  EXPECT_EQ(Library.find("E")->Kind, TypeKind::Enum);
  EXPECT_EQ(Library.find("PE")->Target->Element->Name, "E");
  EXPECT_EQ(recordOf(Library, "_Q").Size, 5u);
  // Microsoft's C gives a union of empty arrays four bytes.
  EXPECT_EQ(recordOf(Library, "_R").Size, 4u);
  EXPECT_EQ(recordOf(Library, "_S").Size, 8u);
  ASSERT_NE(Library.findPrototype("helper"), nullptr);
  ASSERT_NE(Library.findPrototype("quit"), nullptr);
  EXPECT_TRUE(Library.findPrototype("quit")->NoReturn);
  EXPECT_FALSE(Library.findPrototype("helper")->NoReturn);
  ASSERT_NE(Library.findPrototype("renamed"), nullptr);
  ASSERT_NE(Library.findPrototype("info"), nullptr);
  // va_list is Windows' pointer to the arguments.
  const FunctionDef &Log = *Library.findPrototype("logv");
  ASSERT_EQ(Log.Params.size(), 2u);
  EXPECT_EQ(Log.Params[1].Type->Kind, TypeExprKind::Pointer);
}

TEST(TypeParse, RefusesWhatItCannotLayOutAndKeepsGoingWhenAsked) {
  TypeLibrary Library;
  // A #pragma pack of an unknown name leaves every layout after it unknown.
  ParseResult Pack =
      parseCTypes("#pragma pack(push, UNKNOWN)\nstruct A { int a; };", Library);
  ASSERT_EQ(Pack.Problems.size(), 1u);
  EXPECT_NE(Pack.Problems[0].Message.find("UNKNOWN"), std::string::npos);

  const char *Source = "typedef int v4 __attribute__((__vector_size__(16)));\n"
                       "struct B { char c; int i __attribute__((packed)); };\n"
                       "struct C { int c; };\n";
  ParseResult Strict = parseCTypes(Source, Library);
  EXPECT_EQ(Strict.Problems.size(), 2u);
  EXPECT_EQ(Library.find("C"), nullptr);
  ParseOptions Going;
  Going.KeepGoing = true;
  ParseResult Kept = parseCTypes(Source, Library, Going);
  EXPECT_EQ(Kept.Problems.size(), 2u);
  EXPECT_EQ(Library.find("v4"), nullptr);
  EXPECT_EQ(Library.find("B"), nullptr);
  EXPECT_NE(Library.find("C"), nullptr);
}

TEST(TypeParse, ConvertsWithoutAProgram) {
  // A C header read and rendered as Rust and Go, and back.
  TypeLibrary Library;
  ASSERT_TRUE(parseCTypes("typedef LONG NTSTATUS;\n"
                          "typedef struct _PAIR { ULONG Low; LONG High; } "
                          "PAIR, *PPAIR;\n"
                          "NTSTATUS NTAPI Pair(PPAIR Out, ULONG Flags);",
                          Library)
                  .ok());
  for (Language Lang : {Language::Rust, Language::Go, Language::Cpp}) {
    RenderOptions Render;
    Render.Lang = Lang;
    Render.Asserts = true;
    TypeLibrary Back;
    ParseOptions Read;
    Read.Lang = Lang;
    const ParseResult Result =
        parseTypes(renderTypes(Library, {}, Render), Back, Read);
    EXPECT_TRUE(Result.ok()) << languageId(Lang).str();
    EXPECT_EQ(shapeOf(recordOf(Back, "_PAIR")), "struct{Low@0 High@4}");
  }
  RenderOptions Prototypes;
  Prototypes.Prototypes = true;
  const std::string Header = renderTypes(Library, {}, Prototypes);
  EXPECT_NE(Header.find("NTSTATUS Pair(PPAIR Out, ULONG Flags);"),
            std::string::npos);
  TypeLibrary Back;
  ASSERT_TRUE(parseCTypes(Header, Back).ok());
  ASSERT_NE(Back.findPrototype("Pair"), nullptr);
  EXPECT_EQ(Back.findPrototype("Pair")->Params[1].Name, "Flags");
}

TEST(TypeParse, ChecksStaticAssertions) {
  TypeLibrary Library;
  EXPECT_TRUE(parseCTypes(R"(
    struct A { char c; int i; };
    _Static_assert(sizeof(struct A) == 8, "A");
    _Static_assert(offsetof(struct A, i) == 0x4, "A.i");
  )",
                          Library)
                  .ok());
  const ParseResult Wrong = parseCTypes(
      "struct B { int i; };\n_Static_assert(sizeof(struct B) == 3, \"B\");",
      Library);
  ASSERT_EQ(Wrong.Problems.size(), 1u);
  EXPECT_EQ(Wrong.Problems[0].Line, 2u);
  EXPECT_EQ(Wrong.Problems[0].Message, "a static assertion fails: B");
}

TEST(TypeParse, ReportsWhereAndChangesNothingOnAProblem) {
  TypeLibrary Library;
  const ParseResult Result = parseCTypes(
      "struct Good { int a; };\nstruct Bad {\n  DWORD x;\n};", Library);
  ASSERT_EQ(Result.Problems.size(), 1u);
  EXPECT_EQ(Result.Problems[0].Line, 3u);
  EXPECT_EQ(Result.Problems[0].Column, 3u);
  EXPECT_EQ(Result.Problems[0].Message, "'DWORD' names no type");
  // The whole source reads, or none of it.
  EXPECT_EQ(Library.find("Good"), nullptr);
}

TEST(TypeParse, ReportsAProblemOnceInATypedefOfARecord) {
  TypeLibrary Library;
  const ParseResult Result =
      parseCTypes("typedef struct _A {\n  PVOID p;\n  ULONG n;\n} A, *PA;\n"
                  "typedef struct _B { HANDLE h; } B;",
                  Library);
  // Each declaration's first problem, none from the declarators after the
  // body a problem left.
  ASSERT_EQ(Result.Problems.size(), 2u);
  EXPECT_EQ(Result.Problems[0].Line, 2u);
  EXPECT_EQ(Result.Problems[1].Line, 5u);
  EXPECT_EQ(Result.Problems[1].Message, "'HANDLE' names no type");
}

TEST(TypeParse, ReadsCpp) {
  ParseOptions Options;
  Options.Lang = Language::Cpp;
  const TypeLibrary Library = parsed(R"(
    struct alignas(8) Point { int x; int y; };
    enum class Mode : unsigned short { Off, On };
    struct Shape { Point Origin; Mode Kind; };
    static_assert(sizeof(Shape) == 0x10, "Shape");
  )",
                                     Options);
  EXPECT_EQ(shapeOf(recordOf(Library, "Shape")), "struct{Origin@0 Kind@8}");
}

namespace {

/// Renders every type of \p Library as C with layout checks, reads it back,
/// and expects each record to lay out as it did: the same size and members.
void expectRoundTrip(const TypeLibrary &Library, Language Lang,
                     BuiltinSpelling Spelling = BuiltinSpelling::Windows) {
  RenderOptions Render;
  Render.Lang = Lang;
  Render.Spelling = Spelling;
  Render.Asserts = true;
  const std::string Source = renderTypes(Library, {}, Render);
  TypeLibrary Back(Library.dataModel());
  ParseOptions Options;
  Options.Lang = Lang;
  const ParseResult Result = parseTypes(Source, Back, Options);
  for (const SourceProblem &Problem : Result.Problems) {
    // Show the line a problem is on.
    llvm::SmallVector<llvm::StringRef, 0> Lines;
    llvm::StringRef(Source).split(Lines, '\n');
    ADD_FAILURE() << Problem.Line << ":" << Problem.Column << ": "
                  << Problem.Message << "\n  "
                  << (Problem.Line && Problem.Line <= Lines.size()
                          ? Lines[Problem.Line - 1].str()
                          : "");
    return;
  }
  unsigned Records = 0;
  for (const LocalType *Type : Library.types()) {
    if (!Type->Record || !Type->Record->Defined)
      continue;
    ++Records;
    const LocalType *Read = Back.find(Type->Name);
    ASSERT_NE(Read, nullptr) << Type->Name;
    ASSERT_TRUE(Read->Record) << Type->Name;
    EXPECT_EQ(Read->Record->Size, Type->Record->Size) << Type->Name;
    // The renderer's fillers move members and hold no data.
    std::string Expected = leaves(*Type->Record);
    std::string Actual;
    const std::string ReadLeaves = leaves(*Read->Record);
    for (llvm::StringRef Leaf : llvm::split(ReadLeaves, ' '))
      if (!Leaf.empty() && !Leaf.starts_with("__filler") &&
          !Leaf.starts_with("__size@") && !Leaf.starts_with("_filler") &&
          !Leaf.starts_with("_size@"))
        Actual += Leaf.str() + " ";
    EXPECT_EQ(Actual, Expected) << Type->Name;
  }
  EXPECT_GT(Records, 0u);
}

} // namespace

TEST(TypeParse, ReadsRust) {
  ParseOptions Options;
  Options.Lang = Language::Rust;
  const TypeLibrary Library = parsed(R"(
    use core::ffi::c_void;
    /// A device.
    #[repr(C)]
    #[derive(Clone, Copy)]
    pub struct Device {
        pub Kind: u16,
        pub Anonymous: Device_0,
        pub Next: *mut Device,
        pub Dispatch: Option<unsafe extern "system" fn(*mut Device, *mut c_void) -> i32>,
        /// Locked bit 0, Count bits 4..10
        pub _bitfield: u32,
    }
    #[repr(C)]
    pub union Device_0 {
        pub Value: u64,
        pub Bytes: [u8; 8],
    }
    impl Device {
        pub const fn Count(&self) -> i32 { ((self._bitfield << 21) as i32) >> 25 }
    }
    pub type Mode = i32;
    pub const Off: Mode = 0;
    pub const On: Mode = 0x10;
    #[repr(C, packed(1))]
    pub struct Packed { pub Tag: u8, pub Value: u32 }
    const _: () = assert!(core::mem::size_of::<Packed>() == 5);
  )",
                                     Options);
  const RecordDef &Device = recordOf(Library, "Device");
  EXPECT_EQ(shapeOf(Device), "struct{Kind@0 union@8{Value@0 Bytes@0} Next@16 "
                             "Dispatch@24 Locked@256b:1 Count@260b:7}");
  EXPECT_EQ(Library.find("Device_0"), nullptr);
  // The accessor reads Count signed.
  EXPECT_EQ(Device.Members[5].Type->TheBuiltin, Builtin::Int);
  EXPECT_EQ(Library.find("Mode")->Enum->Values[1].Value, 0x10u);
  EXPECT_EQ(recordOf(Library, "Packed").Size, 5u);
}

TEST(TypeParse, ReadsGo) {
  ParseOptions Options;
  Options.Lang = Language::Go;
  const TypeLibrary Library = parsed(R"(
    package types

    import "unsafe"

    // Device: 0x30 (48) bytes.
    type Device struct {
    	Kind      uint16
    	_         [6]byte
    	Anonymous Device_0
    	Next      *Device
    	Dispatch  uintptr
    	_bitfield uint32 // 0x20 Locked bit 0, Count bits 4..10
    	_         [4]byte
    	Name_     [8]byte // 0x28 uint64, unaligned
    }

    // Device_0: the anonymous union at Device+0x08, 0x8 bytes; read it
    // through its accessors.
    type Device_0 struct {
    	raw [1]uint64
    }

    func (s *Device_0) Value() *uint64 { return (*uint64)(unsafe.Pointer(s)) }
    func (s *Device_0) Bytes() *[8]uint8 { return (*[8]uint8)(unsafe.Pointer(s)) }
    func (s *Device) Count() int32 { return int32(s._bitfield<<21) >> 25 }
    func (s *Device) Name() *uint64 { return (*uint64)(unsafe.Pointer(&s.Name_)) }

    type Mode int32

    const (
    	Off Mode = 0
    	On  Mode = 0x10
    )

    // Packed: 0x5 (5) bytes, packed to 1.
    type Packed struct {
    	Tag    uint8
    	Value_ [4]byte // 0x1 uint32, unaligned
    }

    func (s *Packed) Value() *uint32 { return (*uint32)(unsafe.Pointer(&s.Value_)) }

    var _ = [1]int{}[unsafe.Sizeof(Packed{})-0x5]
  )",
                                     Options);
  const RecordDef &Device = recordOf(Library, "Device");
  EXPECT_EQ(shapeOf(Device),
            "struct{Kind@0 union@8{Value@0 Bytes@0} Next@16 Dispatch@24 "
            "Locked@256b:1 Count@260b:7 Name@40}");
  EXPECT_EQ(Library.find("Device_0"), nullptr);
  EXPECT_EQ(Device.Size, 48u);
  // The accessor reads Count signed.
  EXPECT_EQ(Device.Members[5].Type->TheBuiltin, Builtin::Int);
  EXPECT_EQ(Library.find("Mode")->Enum->Values[1].Value, 0x10u);
  const RecordDef &Packed = recordOf(Library, "Packed");
  EXPECT_EQ(Packed.Size, 5u);
  EXPECT_EQ(shapeOf(Packed), "struct{Tag@0 Value@1}");
}

TEST(TypeParse, ReadsBackTheFixtureAsRendered) {
  auto Library = importPDBTypes(NEVERD_TYPES_FIXTURES "/local_types.pdb");
  ASSERT_TRUE(bool(Library)) << llvm::toString(Library.takeError());
  for (BuiltinSpelling Spelling :
       {BuiltinSpelling::Windows, BuiltinSpelling::IDA,
        BuiltinSpelling::Keywords, BuiltinSpelling::Portable})
    expectRoundTrip(**Library, Language::C, Spelling);
  expectRoundTrip(**Library, Language::Cpp);
  expectRoundTrip(**Library, Language::Rust);
  expectRoundTrip(**Library, Language::Go);
}

TEST(TypeParseKernel, ReadsBackEveryKernelTypeAsRendered) {
  const char *Path = std::getenv("NEVERD_TYPES_NTKRNLMP");
  if (!Path)
    GTEST_SKIP() << "NEVERD_TYPES_NTKRNLMP is not set";
  auto Library = importPDBTypes(Path);
  ASSERT_TRUE(bool(Library)) << llvm::toString(Library.takeError());
  expectRoundTrip(**Library, Language::C);
  expectRoundTrip(**Library, Language::Rust);
  expectRoundTrip(**Library, Language::Go);
}
