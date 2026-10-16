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

  // Records of another size, and members given already, stay as they are.
  const TypeRef Smaller = NdType::makeNamedRecord("_HEADER", 4);
  Context->completeType(Smaller);
  EXPECT_TRUE(Smaller->FieldDisplayNames.empty());
  const TypeRef Own = NdType::makeNamedRecord("_LARGE", 8);
  Own->FieldDisplayNames = {"Theirs"};
  Own->FieldDisplayOffsets = {0};
  Own->FieldDisplayTypes = {NdType::makeInt(8, true)};
  Context->completeType(Own);
  EXPECT_EQ(Own->FieldDisplayNames, std::vector<std::string>{"Theirs"});

  // A unit holds the records whose members it names as the library does,
  // and what they hold by value; one named otherwise stays declared.
  const CRecordDefinitions Defined = Context->definitions({Holder, Own});
  EXPECT_NE(Defined.Text.find("struct _HOLDER\n{"), std::string::npos)
      << Defined.Text;
  EXPECT_NE(Defined.Text.find("#ifndef NEVERD_TYPE__HOLDER"),
            std::string::npos);
  EXPECT_EQ(Defined.Records.count("_HOLDER"), 1u);
  EXPECT_TRUE(Defined.Records.at("_LARGE")) << "held by value, a union";
  const CRecordDefinitions Otherwise = Context->definitions({Own});
  EXPECT_TRUE(Otherwise.Text.empty());

  // The context takes back what it gave, for the next to give anew.
  Context.reset();
  EXPECT_TRUE(Holder->FieldDisplayNames.empty());
  EXPECT_EQ(Own->FieldDisplayNames, std::vector<std::string>{"Theirs"});
}
