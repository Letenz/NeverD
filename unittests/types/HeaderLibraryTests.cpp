//===- HeaderLibraryTests.cpp - Type libraries of system headers ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/types/HeaderLibrary.h"
#include "neverd/types/TypeParse.h"

using namespace neverd::types;

namespace {

std::optional<uint64_t> sizeOf(const TypeLibrary &Library,
                               llvm::StringRef Name) {
  const LocalType *Type = Library.find(Name);
  return Type ? Library.sizeOf(*makeNamed(Name.str(), Type->Kind))
              : std::nullopt;
}

} // namespace

TEST(HeaderLibrary, TheKernelsReadsAsADriverDeclaresIt) {
  std::string Error;
  const TypeLibrary *Kernel =
      headerLibrary(HeaderLibrary::WindowsKernelX64, &Error);
  ASSERT_NE(Kernel, nullptr) << Error;
  EXPECT_EQ(Kernel->dataModel(), DataModel::LLP64);
  // Read once: the same library every time.
  EXPECT_EQ(headerLibrary(HeaderLibrary::WindowsKernelX64), Kernel);
  EXPECT_GT(Kernel->size(), 3000u);
  EXPECT_GT(Kernel->prototypes().size(), 1500u);
  for (const LocalType *Type : Kernel->types())
    EXPECT_EQ(Type->Origin, TypeOrigin::Header) << Type->Name;

  // The sizes x64 Windows gives them.
  EXPECT_EQ(sizeOf(*Kernel, "UNICODE_STRING"), 16u);
  EXPECT_EQ(sizeOf(*Kernel, "OBJECT_ATTRIBUTES"), 48u);
  EXPECT_EQ(sizeOf(*Kernel, "IO_STATUS_BLOCK"), 16u);
  EXPECT_EQ(sizeOf(*Kernel, "LIST_ENTRY"), 16u);
  EXPECT_EQ(sizeOf(*Kernel, "NTSTATUS"), 4u);
  EXPECT_EQ(sizeOf(*Kernel, "HANDLE"), 8u);
  EXPECT_EQ(Kernel->alignOf(*makeNamed("SLIST_ENTRY", TypeKind::Typedef)), 16u);

  // Prototypes as the WDK declares them, parameters named.
  const FunctionDef *Create = Kernel->findPrototype("NtCreateFile");
  ASSERT_NE(Create, nullptr);
  ASSERT_EQ(Create->Params.size(), 11u);
  EXPECT_EQ(Create->Params[0].Name, "FileHandle");
  EXPECT_EQ(Create->Params[10].Name, "EaLength");
  EXPECT_EQ(Create->Return->Name, "NTSTATUS");
  ASSERT_NE(Kernel->findPrototype("KeBugCheckEx"), nullptr);
  EXPECT_TRUE(Kernel->findPrototype("KeBugCheckEx")->NoReturn);
  EXPECT_FALSE(Kernel->findPrototype("IoCreateDevice")->NoReturn);
}

TEST(HeaderLibrary, JoinsWhereTheProgramHasNoTypeOfTheName) {
  TypeLibrary Headers;
  ParseOptions Options;
  Options.Origin = TypeOrigin::Header;
  ASSERT_TRUE(parseCTypes(R"(
    typedef struct _OBJ { ULONG A; } OBJ, *POBJ;
    typedef struct _HOLDER { struct _OBJ Obj; ULONG Tail; } HOLDER;
    typedef struct _USER { struct _HOLDER Holder; } USER;
    typedef struct _POINTS { POBJ First; } POINTS;
    VOID Use(POBJ Object);
  )",
                          Headers, Options)
                  .ok());
  // The program's _OBJ, of another size, and a prototype of its own.
  TypeLibrary Program;
  ASSERT_TRUE(parseCTypes("struct _OBJ { ULONGLONG A, B; };\n"
                          "ULONG Use(void *Object);",
                          Program)
                  .ok());
  const HeaderMerge Merge = mergeHeaderTypes(Program, Headers);
  // _HOLDER was laid out with the headers' _OBJ, and _USER holds it: both
  // go, with the typedefs that name them.  OBJ names the program's _OBJ,
  // and _POINTS only points to it.
  EXPECT_EQ(Merge.Conflicts,
            (std::vector<std::string>{"_HOLDER", "HOLDER", "_USER", "USER"}));
  EXPECT_EQ(Program.find("_HOLDER"), nullptr);
  EXPECT_EQ(Program.find("HOLDER"), nullptr);
  EXPECT_EQ(sizeOf(Program, "OBJ"), 16u);
  EXPECT_NE(Program.find("_POINTS"), nullptr);
  EXPECT_EQ(Program.find("_POINTS")->Origin, TypeOrigin::Header);
  EXPECT_EQ(Program.find("_OBJ")->Origin, TypeOrigin::User);
  // The program's own prototype stays.
  EXPECT_EQ(Program.findPrototype("Use")->Return->TheBuiltin, Builtin::ULong);
  EXPECT_EQ(Merge.Prototypes, 0u);
}
