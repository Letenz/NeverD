//===- LocalTypesDebugContextTests.cpp - Local types context --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "LocalTypesDebugContext.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/types/TypeParse.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

/// Debug information that names functions and types none, as a PDB of
/// public symbols does.
class NamesOnlyDebugContext final : public neverd::DebugContext {
public:
  std::map<neverd::va_t, neverd::FunctionSym> Functions;
  std::optional<neverd::FunctionSym>
  resolveFunction(neverd::va_t Addr) const override {
    for (const auto &[Entry, Function] : Functions)
      if (Function.contains(Addr) || Entry == Addr)
        return Function;
    return std::nullopt;
  }
  std::optional<neverd::VariableSym> resolveVariable(neverd::va_t,
                                                     int64_t) const override {
    return std::nullopt;
  }
  std::optional<neverd::TypeSym> resolveType(uint64_t) const override {
    return std::nullopt;
  }
  std::optional<neverd::SourceLoc> sourceLocation(neverd::va_t) const override {
    return std::nullopt;
  }
  std::vector<neverd::FunctionSym> allFunctions() const override { return {}; }
  bool hasInfo() const override { return true; }
};

} // namespace

TEST(LocalTypesDebugContext, TypesTheFunctionsDebugInformationLeavesUntyped) {
  using namespace neverd;
  auto Library = std::make_shared<types::TypeLibrary>();
  ASSERT_TRUE(types::parseCTypes(R"(
    typedef LONG NTSTATUS;
    typedef struct _PAIR { ULONG Low; union { LONG High; ULONG Top; }; } PAIR,
        *PPAIR;
    typedef VOID (NTAPI *PCALLBACK)(PPAIR Pair, ULONG Flags);
    NTSTATUS NTAPI Pair(PPAIR Out, ULONG Flags, PCALLBACK Done);
    PPAIR NTAPI NextPair(PPAIR Current);
    ULONG __cdecl Print(const char *Format, ...);
  )",
                                 *Library)
                  .ok());
  NamesOnlyDebugContext Names;
  const auto name = [&](va_t Addr, const char *Name) {
    FunctionSym Function;
    Function.Name = Name;
    Function.Addr = Addr;
    Function.Size = 0x10;
    Names.Functions[Addr] = Function;
  };
  name(0x1000, "Pair");
  name(0x2000, "NextPair");
  name(0x3000, "Print");
  name(0x4000, "Unknown");
  FunctionSym Typed;
  Typed.Name = "Pair";
  Typed.Addr = 0x5000;
  Typed.Size = 0x10;
  Typed.ReturnType = NdType::makeInt(8, false);
  Names.Functions[0x5000] = Typed;
  sdk::LocalTypesDebugContext Context(Names, Library);

  std::optional<FunctionSym> Pair = Context.resolveFunction(0x1000);
  ASSERT_TRUE(Pair.has_value());
  ASSERT_NE(Pair->ReturnType, nullptr);
  EXPECT_EQ(Pair->ReturnType->Kind, NdTypeKind::Int);
  EXPECT_EQ(Pair->ReturnType->Size, 4u);
  EXPECT_TRUE(Pair->ReturnType->IsSigned);
  ASSERT_EQ(Pair->Params.size(), 3u);
  EXPECT_EQ(Pair->Params[0].first, "Out");
  // The record pointed to has its name and its members' names, those of
  // the union in place among them; it is C's.
  const TypeRef &Record = Pair->Params[0].second->Pointee;
  EXPECT_EQ(Record->SourceName, "_PAIR");
  EXPECT_EQ(Record->Size, 8u);
  EXPECT_EQ(Record->Passing, NdRecordPassing::ByValue);
  EXPECT_EQ(Record->FieldDisplayNames,
            (std::vector<std::string>{"Low", "High", "Top"}));
  EXPECT_EQ(Record->FieldDisplayOffsets, (std::vector<uint16_t>{0, 4, 4}));
  // A callback is a pointer to a function of opaque pointers and scalars.
  const TypeRef &Done = Pair->Params[2].second;
  ASSERT_EQ(Done->Kind, NdTypeKind::Ptr);
  ASSERT_EQ(Done->Pointee->Kind, NdTypeKind::Func);
  ASSERT_EQ(Done->Pointee->ParamTypes.size(), 2u);
  EXPECT_EQ(Done->Pointee->ParamTypes[0]->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(Done->Pointee->ParamTypes[0]->Pointee->Kind, NdTypeKind::Void);

  // A record returned by a C declaration's pointer is no hidden result.
  std::optional<FunctionSym> Next = Context.resolveFunction(0x2000);
  ASSERT_TRUE(Next.has_value());
  EXPECT_FALSE(
      isMsvcIndirectReturn(Next->ReturnType, Arch::X64, BinaryFormat::COFF));
  // A variadic prototype's calls pass more than it declares; a name
  // without one, an interior address and a typed declaration stay.
  EXPECT_TRUE(Context.resolveFunction(0x3000)->Params.empty());
  EXPECT_TRUE(Context.resolveFunction(0x4000)->Params.empty());
  EXPECT_TRUE(Context.resolveFunction(0x1008)->Params.empty());
  EXPECT_TRUE(Context.resolveFunction(0x5000)->Params.empty());
  EXPECT_EQ(Context.resolveFunction(0x5000)->ReturnType->Size, 8u);
}

TEST(LocalTypesDebugContext, TypesTheFunctionsByTheNamesTheyAreShownUnder) {
  using namespace neverd;
  auto Library = std::make_shared<types::TypeLibrary>();
  ASSERT_TRUE(types::parseCTypes(R"(
    typedef struct _THREAD *PTHREAD;
    typedef struct _PROCESS *PPROCESS;
    PPROCESS NTAPI ThreadToProcess(PTHREAD Thread);
    ULONG NTAPI Exported(ULONG Value);
    ULONG NTAPI Stated(ULONG Value, ULONG Shift);
    ULONG NTAPI Analyzed(ULONG Value);
    ULONG NTAPI Folded(ULONG Value, void *Context);
    ULONG NTAPI Debugged(ULONG Value);
    ULONG NTAPI Fewer(ULONG Value);
    ULONG NTAPI More(ULONG Value, ULONG Extra);
    LONG NTAPI Signed(PTHREAD Thread);
    ULONG NTAPI Unsigned(PPROCESS Process);
    ULONG NTAPI Recognized(ULONG Value, ULONG Other);
  )",
                                 *Library)
                  .ok());
  BinaryImage Image;
  // A linker that folds identical functions exports one entry under each
  // of their names: the first names the function shown there.
  Image.Exports = {{"ThreadToProcess", 1, 0x1000}, {"Unprototyped", 2, 0x2000},
                   {"Folded", 3, 0x2000},          {"Exported", 4, 0x6000},
                   {"Fewer", 5, 0x7000},           {"More", 6, 0x7000},
                   {"Signed", 7, 0x8000},          {"Unsigned", 8, 0x8000}};
  const auto symbol = [&](const char *Name, va_t Addr, NameOrigin Origin) {
    Symbol Sym;
    Sym.Name = Name;
    Sym.Addr = Addr;
    Sym.IsFunc = true;
    Sym.Origin = Origin;
    Image.Symbols.push_back(Sym);
  };
  symbol("Stated", 0x3000, NameOrigin::Stated);
  symbol("Analyzed", 0x4000, NameOrigin::Analysis);
  NamesOnlyDebugContext Names;
  const auto name = [&](va_t Addr, const char *Name) {
    FunctionSym Function;
    Function.Name = Name;
    Function.Addr = Addr;
    Function.Size = 0x10;
    Names.Functions[Addr] = Function;
  };
  name(0x5000, "Debugged");
  name(0x6000, "Debugged");
  // Signatures recognize what nothing else names; a stated name comes first.
  sdk::LocalTypesDebugContext Context(
      Names, Library, {}, &Image, {},
      {{0x9000, "Recognized"}, {0x1000, "Recognized"}});

  // An export the debug information does not name takes its prototype,
  // with what declares it.
  std::optional<FunctionSym> Owner = Context.resolveFunction(0x1000);
  ASSERT_TRUE(Owner.has_value());
  EXPECT_EQ(Owner->Addr, 0x1000u);
  ASSERT_EQ(Owner->Params.size(), 1u);
  EXPECT_EQ(Owner->Params[0].first, "Thread");
  ASSERT_NE(Owner->ReturnType, nullptr);
  EXPECT_EQ(Owner->ReturnType->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(Owner->PrototypeLibrary, "local-types");
  EXPECT_EQ(Owner->PrototypeDeclaration,
            "PPROCESS ThreadToProcess(PTHREAD Thread)");
  // The function folded with another is shown under its first name, which
  // has no prototype: the other name's would describe another function.
  EXPECT_FALSE(Context.resolveFunction(0x2000).has_value());
  // A symbol the program states names a function as an export does; one
  // that analysis made up does not.
  ASSERT_TRUE(Context.resolveFunction(0x3000).has_value());
  EXPECT_EQ(Context.resolveFunction(0x3000)->Params.size(), 2u);
  EXPECT_FALSE(Context.resolveFunction(0x4000).has_value());
  // Where the image states no name, the debug information's name does; the
  // image's comes first, as the function is shown under it.
  std::optional<FunctionSym> Debugged = Context.resolveFunction(0x5000);
  ASSERT_TRUE(Debugged.has_value());
  EXPECT_EQ(Debugged->Name, "Debugged");
  EXPECT_EQ(Debugged->Size, 0x10u);
  EXPECT_EQ(Debugged->Params.size(), 1u);
  std::optional<FunctionSym> Exported = Context.resolveFunction(0x6000);
  ASSERT_TRUE(Exported.has_value());
  // A builtin, where no header names it, in exact widths.
  EXPECT_EQ(Exported->PrototypeDeclaration,
            "uint32_t Exported(uint32_t Value)");
  // An address inside a function names no function of its own.
  EXPECT_TRUE(Context.resolveFunction(0x5008)->Params.empty());
  // Folded functions whose prototypes take different parameters: which the
  // code reads is unknown, and so it takes neither.
  EXPECT_FALSE(Context.resolveFunction(0x7000).has_value());
  // Ones that pass and return alike, whatever they point at or sign: the
  // shown name's.
  std::optional<FunctionSym> Signed = Context.resolveFunction(0x8000);
  ASSERT_TRUE(Signed.has_value());
  ASSERT_EQ(Signed->Params.size(), 1u);
  EXPECT_EQ(Signed->Params[0].first, "Thread");
  EXPECT_TRUE(Signed->ReturnType->IsSigned);
  std::optional<FunctionSym> Recognized = Context.resolveFunction(0x9000);
  ASSERT_TRUE(Recognized.has_value());
  EXPECT_EQ(Recognized->Params.size(), 2u);
}

TEST(LocalTypesDebugContext, NamesTheHeaderLibraryAPrototypeComesFrom) {
  using namespace neverd;
  const types::TypeLibrary *Kernel =
      types::headerLibrary(types::HeaderLibrary::WindowsKernelX64);
  ASSERT_NE(Kernel, nullptr);
  auto Library = std::make_shared<types::TypeLibrary>(Kernel->dataModel());
  types::mergeHeaderTypes(*Library, *Kernel);
  BinaryImage Image;
  Image.Exports = {{"IoThreadToProcess", 1, 0x1000}};
  NamesOnlyDebugContext Names;
  sdk::LocalTypesDebugContext Context(Names, Library, {}, &Image,
                                      {types::HeaderLibrary::WindowsKernelX64});
  std::optional<FunctionSym> Owner = Context.resolveFunction(0x1000);
  ASSERT_TRUE(Owner.has_value());
  EXPECT_EQ(Owner->PrototypeLibrary, "windows-kernel-x64");
  EXPECT_EQ(Owner->PrototypeDeclaration,
            "PEPROCESS IoThreadToProcess(PETHREAD Thread)");
  ASSERT_EQ(Owner->Params.size(), 1u);
  EXPECT_EQ(Owner->Params[0].first, "Thread");
}

TEST(LocalTypesDebugContext, GivesTheDebugInformationsRecordsTheirMembers) {
  using namespace neverd;
  auto Library = std::make_shared<types::TypeLibrary>();
  ASSERT_TRUE(types::parseCTypes(R"(
    typedef union _LARGE {
      struct { ULONG Low; LONG High; };
      struct { ULONG Low; LONG High; } u;
      LONGLONG Quad;
    } LARGE;
    typedef struct _HEADER {
      union {
        struct { UCHAR Type; UCHAR Absolute; };
        struct { UCHAR TimerType; UCHAR TimerFlags; };
        LONG Lock;
      };
      ULONG Signal;
    } HEADER;
    typedef struct _HOLDER { HEADER Header; LARGE Time; void *Next; } HOLDER;
  )",
                                 *Library)
                  .ok());
  NamesOnlyDebugContext Names;
  auto Context = std::make_unique<sdk::LocalTypesDebugContext>(Names, Library);
  // A record the debug information names but gives no members takes the
  // library's.
  const TypeRef Holder = NdType::makeNamedRecord("_HOLDER", 24);
  Context->completeType(NdType::makePtr(Holder));
  ASSERT_FALSE(Holder->FieldDisplayNames.empty());
  // An eight-byte read of the union in place is its scalar, not the record
  // of the same size; a byte of the header names no one member of the
  // union's alternatives, and is no header.
  EXPECT_EQ(Holder->displayFieldPathAt(8, 8), "Time.Quad");
  EXPECT_EQ(Holder->displayFieldPathAt(0, 1), std::nullopt);
  EXPECT_EQ(Holder->displayFieldPathAt(4, 4), "Header.Signal");
  EXPECT_EQ(Holder->displayFieldPathAt(16, 8), "Next");
  EXPECT_TRUE(Context->readTypes().contains("_HOLDER"));

  // A record of another size stays as it is.
  const TypeRef Smaller = NdType::makeNamedRecord("_HEADER", 4);
  Context->completeType(Smaller);
  EXPECT_TRUE(Smaller->FieldDisplayNames.empty());
  // Members the debug information gave where the library's lie take the
  // library's names, as those the user renamed there do; members that lie
  // where the library's do not, as a C++ base's flattened into its class,
  // stay.
  const TypeRef Own = NdType::makeNamedRecord("_LARGE", 8);
  Own->FieldDisplayNames = {"Theirs"};
  Own->FieldDisplayOffsets = {0};
  Own->FieldDisplayTypes = {NdType::makeInt(8, true)};
  Context->completeType(Own);
  EXPECT_EQ(Own->displayFieldPathAt(0, 8), "Quad");
  EXPECT_EQ(Own->displayFieldPathAt(4, 4), "High");
  const TypeRef Elsewhere = NdType::makeNamedRecord("_LARGE", 8);
  Elsewhere->FieldDisplayNames = {"Theirs"};
  Elsewhere->FieldDisplayOffsets = {2};
  Elsewhere->FieldDisplayTypes = {NdType::makeInt(2, true)};
  Context->completeType(Elsewhere);
  EXPECT_EQ(Elsewhere->FieldDisplayNames, std::vector<std::string>{"Theirs"});

  // A unit holds the records whose members it names as the library does,
  // and what they hold by value; one named otherwise stays declared.
  const CRecordDefinitions Defined = Context->definitions({Holder, Own});
  EXPECT_NE(Defined.Text.find("struct _HOLDER\n{"), std::string::npos)
      << Defined.Text;
  EXPECT_NE(Defined.Text.find("#ifndef NEVERD_TYPE__HOLDER"),
            std::string::npos);
  EXPECT_EQ(Defined.Records.count("_HOLDER"), 1u);
  EXPECT_TRUE(Defined.Records.at("_LARGE")) << "held by value, a union";
  const CRecordDefinitions Otherwise = Context->definitions({Elsewhere});
  EXPECT_TRUE(Otherwise.Text.empty());

  // The context takes back what it gave, for the next to give anew.
  Context.reset();
  EXPECT_TRUE(Holder->FieldDisplayNames.empty());
  EXPECT_EQ(Own->FieldDisplayNames, std::vector<std::string>{"Theirs"});
  EXPECT_EQ(Own->FieldDisplayOffsets, std::vector<uint16_t>{0});
}

TEST(LocalTypesDebugContext, NamesValuesByTheLibrarysEnumerators) {
  using namespace neverd;
  auto Library = std::make_shared<types::TypeLibrary>();
  ASSERT_TRUE(types::parseCTypes(R"(
    typedef enum _COLOR { Red, Green = 5, Huge = 0x10000, Below = -1 } COLOR;
    typedef struct _PAINT { COLOR Color; ULONG Count; } PAINT;
  )",
                                 *Library)
                  .ok());
  NamesOnlyDebugContext Names;
  auto Context = std::make_unique<sdk::LocalTypesDebugContext>(Names, Library);
  // A record's member of an enumeration is the enumeration, its values
  // named by its enumerators: those a display holds, none negative and
  // none wider than 16 bits.
  const TypeRef Paint = NdType::makeNamedRecord("_PAINT", 8);
  Context->completeType(Paint);
  ASSERT_EQ(Paint->FieldDisplayTypes.size(), 2u);
  const TypeRef Color = Paint->FieldDisplayTypes[0];
  ASSERT_TRUE(Color && Color->IsEnum);
  EXPECT_EQ(Color->SourceName, "_COLOR");
  EXPECT_EQ(Color->Size, 4);
  EXPECT_TRUE(Color->IsSigned);
  EXPECT_EQ(Color->displayFieldNameAt(5), "Green");
  EXPECT_EQ(Color->FieldDisplayNames,
            (std::vector<std::string>{"Red", "Green"}));

  // An enumeration the debug information gave enumerators takes the
  // library's, which the user may have renamed, and gets its own back.
  const TypeRef Given = NdType::makeNamedRecord("_COLOR", 0, /*Enum=*/true);
  Given->FieldDisplayNames = {"Rouge", "Vert"};
  Given->FieldDisplayOffsets = {0, 5};
  Context->completeType(Given);
  EXPECT_EQ(Given->displayFieldNameAt(5), "Green");
  EXPECT_TRUE(Context->readTypes().contains("_COLOR"));
  // A unit holds the enumeration, its enumerators agreeing.
  const CRecordDefinitions Defined = Context->definitions({Given});
  EXPECT_NE(Defined.Text.find("enum _COLOR\n{"), std::string::npos)
      << Defined.Text;
  EXPECT_EQ(Defined.Enumerations, std::vector<std::string>{"_COLOR"});
  Context.reset();
  EXPECT_EQ(Given->FieldDisplayNames,
            (std::vector<std::string>{"Rouge", "Vert"}));
}
