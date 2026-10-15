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
  EXPECT_EQ(Dispatch.Element->Function->Convention, CallingConvention::Stdcall);
  EXPECT_EQ(Dispatch.Element->Function->Params[0].Name, "Self");

  const FunctionDef *Call = Library.findPrototype("IoCallDevice");
  ASSERT_NE(Call, nullptr);
  EXPECT_EQ(Call->Convention, CallingConvention::Stdcall);
  ASSERT_EQ(Call->Params.size(), 2u);
  EXPECT_EQ(Call->Params[1].Name, "Length");
  EXPECT_TRUE(Call->Variadic);
  EXPECT_EQ(Library.find("PROUTINE")->Target->Element->Kind,
            TypeExprKind::Function);
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
