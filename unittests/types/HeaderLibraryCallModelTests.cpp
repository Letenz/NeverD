//===- HeaderLibraryCallModelTests.cpp - Libraries and the call model -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The call model's tables (lib/libc) answer what a routine takes and
/// returns before any header library joins a program, and where none does;
/// the header libraries answer it for a program they join.  For every
/// routine both know, they must agree on what the machine passes and
/// returns.
///
//===----------------------------------------------------------------------===//

#include "neverd/libc/LibCNames.h"
#include "neverd/libc/LibCResult.h"
#include "neverd/loader/SymbolDecoration.h"
#include "neverd/types/HeaderLibrary.h"
#include "neverd/types/LocalTypes.h"

#include <gtest/gtest.h>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace neverd;
using namespace neverd::types;

namespace {

/// What the machine passes or returns for a type.
enum class Kind {
  Void,
  Integer,
  Pointer,
  FunctionPointer,
  Float,
  Record,
  Other
};

struct Shape {
  Kind K = Kind::Other;
  uint64_t Bytes = 0;
};

const TypeExpr *throughTypedefs(const TypeLibrary &Library,
                                const TypeExpr *Type) {
  for (unsigned Depth = 0; Type && Depth < 32; ++Depth) {
    if (Type->Kind != TypeExprKind::Named)
      return Type;
    const LocalType *Named = Library.find(Type->Name);
    if (!Named || Named->Kind != TypeKind::Typedef || !Named->Target)
      return Type;
    Type = Named->Target.get();
  }
  return Type;
}

Shape shapeOf(const TypeLibrary &Library, const TypeExpr &Expr) {
  const TypeExpr *Type = throughTypedefs(Library, &Expr);
  if (!Type)
    return {};
  const uint64_t Size = Library.sizeOf(*Type).value_or(0);
  switch (Type->Kind) {
  case TypeExprKind::Builtin: {
    const BuiltinClass Class = builtinInfo(Type->TheBuiltin).Class;
    if (Class == BuiltinClass::Void)
      return {Kind::Void, 0};
    if (Class == BuiltinClass::Float)
      return {Kind::Float, Size};
    return {Kind::Integer, Size};
  }
  case TypeExprKind::Named: {
    const LocalType *Named = Library.find(Type->Name);
    if (!Named)
      return {};
    if (Named->Kind == TypeKind::Enum)
      return {Kind::Integer, Size};
    // A union of object pointers alone passes as the pointer it holds:
    // glibc's __CONST_SOCKADDR_ARG, a transparent union that takes a
    // sockaddr pointer of any family.
    if (Named->Kind == TypeKind::Union && Named->Record &&
        !Named->Record->Members.empty() &&
        llvm::all_of(Named->Record->Members, [&](const Member &M) {
          return M.Type && shapeOf(Library, *M.Type).K == Kind::Pointer;
        }))
      return {Kind::Pointer, Size};
    if (Named->Kind == TypeKind::Struct || Named->Kind == TypeKind::Union)
      return {Kind::Record, Size};
    return {};
  }
  case TypeExprKind::Pointer: {
    const TypeExpr *Pointee = throughTypedefs(Library, Type->Element.get());
    const LocalType *Named = Pointee && Pointee->Kind == TypeExprKind::Named
                                 ? Library.find(Pointee->Name)
                                 : nullptr;
    if ((Pointee && Pointee->Kind == TypeExprKind::Function) ||
        (Named && Named->Kind == TypeKind::Function))
      return {Kind::FunctionPointer, Type->PointerBytes};
    return {Kind::Pointer, Type->PointerBytes};
  }
  case TypeExprKind::Array:
    return {Kind::Pointer, 0};
  case TypeExprKind::Function:
    return {Kind::FunctionPointer, 0};
  case TypeExprKind::Record:
    return {Kind::Record, Size};
  case TypeExprKind::Enum:
    return {Kind::Integer, Size};
  }
  return {};
}

bool isPointer(Kind K) {
  return K == Kind::Pointer || K == Kind::FunctionPointer;
}

/// The object format of the programs \p Library describes.
BinaryFormat formatOf(HeaderLibrary Library) {
  const llvm::StringRef Name = headerLibraryName(Library);
  if (Name.starts_with("windows-") || Name.starts_with("uefi-"))
    return BinaryFormat::COFF;
  if (Name.starts_with("macos-") || Name.starts_with("ios-"))
    return BinaryFormat::MachO;
  return BinaryFormat::ELF;
}

/// The C library of the programs \p Library describes.
libc::LibCVariant variantOf(HeaderLibrary Library) {
  const llvm::StringRef Name = headerLibraryName(Library);
  if (Name.starts_with("android-"))
    return libc::LibCVariant::Bionic;
  if (Name.starts_with("musl-"))
    return libc::LibCVariant::Musl;
  if (Name.starts_with("freebsd-"))
    return libc::LibCVariant::FreeBSD;
  return libc::libcVariantOf(formatOf(Library));
}

/// The symbol a program of \p Format imports the C routine \p Name by.
std::string symbolOf(llvm::StringRef Name, BinaryFormat Format) {
  return (Format == BinaryFormat::MachO ? "_" : "") + Name.str();
}

struct Disagreement {
  std::string Check;
  std::string Routine;
  std::string Library;
  std::string Detail;
};

std::vector<Disagreement> disagreements() {
  std::vector<Disagreement> Out;
  const HeaderLibrary Libraries[] = {
#define NEVERD_HEADER_LIBRARY(Id, Name, File, Model, ABI, Spelling)            \
  HeaderLibrary::Id,
#include "neverd/types/HeaderLibraries.def"
  };
  for (const HeaderLibrary Library : Libraries) {
    const TypeLibrary *Types = headerLibrary(Library);
    if (!Types)
      continue;
    const BinaryFormat Format = formatOf(Library);
    const libc::LibCVariant Variant = variantOf(Library);
    const std::string LibraryName = headerLibraryName(Library).str();
    for (const auto &Entry : Types->prototypes()) {
      const std::string Name = Entry.getKey().str();
      const FunctionDef &Function = *Entry.getValue();
      const auto Note = [&](const char *Check, std::string Detail) {
        Out.push_back({Check, Name, LibraryName, std::move(Detail)});
      };
      const Shape Return = Function.Return ? shapeOf(*Types, *Function.Return)
                                           : Shape{Kind::Void, 0};
      std::vector<Shape> Params;
      for (const Parameter &Param : Function.Params)
        Params.push_back(Param.Type ? shapeOf(*Types, *Param.Type) : Shape{});
      const uint16_t ReturnFloat =
          Return.K == Kind::Float && (Return.Bytes == 4 || Return.Bytes == 8)
              ? Return.Bytes
              : 0;

      if (const libc::LibCPrototype *Prototype =
              libc::libcPrototype(Name, Format)) {
        if (Prototype->Variadic != Function.Variadic)
          Note("prototype-variadic", std::to_string(Prototype->Variadic) +
                                         " vs " +
                                         std::to_string(Function.Variadic));
        if (Prototype->ParamCount != Params.size())
          Note("prototype-params", std::to_string(Prototype->ParamCount) +
                                       " vs " + std::to_string(Params.size()));
        if ((Prototype->Return == "void") != (Return.K == Kind::Void))
          Note("prototype-void", std::string(Prototype->Return));
        if (libc::isPointerType(Prototype->Return) != isPointer(Return.K))
          Note("prototype-pointer", std::string(Prototype->Return));
        if (libc::floatReturnBytes(*Prototype) != ReturnFloat)
          Note("prototype-float-return",
               std::to_string(libc::floatReturnBytes(*Prototype)) + " vs " +
                   std::to_string(ReturnFloat));
        for (unsigned I = 0; I < Prototype->ParamCount && I < Params.size();
             ++I)
          if (libc::isFloatingType(Prototype->Params[I]) !=
              (Params[I].K == Kind::Float))
            Note("prototype-param-float",
                 "param " + std::to_string(I) + " " +
                     std::string(Prototype->Params[I]));
      }

      const std::string Symbol = symbolOf(Name, Format);
      const std::optional<libc::LibCArity> Arity =
          importNamesAreCNames(Format)
              ? libc::libcArity(Symbol, Variant)
              : libc::libcArityForSymbol(Symbol, Variant);
      if (Arity && Arity->IntArgs >= 0) {
        int Integers = 0, Floats = 0;
        for (const Shape &Param : Params)
          (Param.K == Kind::Float ? Floats : Integers) += 1;
        if (!Function.Variadic &&
            (Arity->IntArgs != Integers || Arity->FpArgs != Floats))
          Note("arity", std::to_string(Arity->IntArgs) + "+" +
                            std::to_string(Arity->FpArgs) + " vs " +
                            std::to_string(Integers) + "+" +
                            std::to_string(Floats));
        if (Function.Variadic &&
            Arity->IntArgs + Arity->FpArgs != static_cast<int>(Params.size()))
          Note("arity-variadic", std::to_string(Arity->IntArgs) + "+" +
                                     std::to_string(Arity->FpArgs) + " vs " +
                                     std::to_string(Params.size()) + " fixed");
        if (libc::floatReturnBytes(*Arity) != ReturnFloat &&
            !Arity->FpRetLongDouble && !Arity->FpRetComplex)
          Note("arity-float-return",
               std::to_string(libc::floatReturnBytes(*Arity)) + " vs " +
                   std::to_string(ReturnFloat));
      }

      if (const std::optional<bool> Returns =
              libc::libcReturnsValue(Name, Variant);
          Returns && *Returns != (Return.K != Kind::Void))
        Note("returns-value", std::to_string(*Returns));
      if (const std::optional<libc::LibCResult> Result =
              libc::libcResult(Name, Variant)) {
        const libc::LibCResult Header =
            Return.K == Kind::Void ? libc::LibCResult::Void
            : isPointer(Return.K)  ? libc::LibCResult::Pointer
                                   : libc::LibCResult::Value;
        if (*Result != Header)
          Note("result", std::to_string(static_cast<int>(*Result)) + " vs " +
                             std::to_string(static_cast<int>(Header)));
      }

      for (unsigned I = 0; I < Params.size(); ++I) {
        if (libc::isObjectPointerParameter(Name, I, Variant) &&
            Params[I].K != Kind::Pointer)
          Note("object-pointer-param", "param " + std::to_string(I));
        if (libc::functionPointerParameter(Name, I, Variant) &&
            Params[I].K != Kind::FunctionPointer)
          Note("function-pointer-param", "param " + std::to_string(I));
        if (libc::isCStringParameter(Name, I) && !isPointer(Params[I].K))
          Note("string-param", "param " + std::to_string(I));
      }
      if (Function.Variadic)
        if (const unsigned Fixed = libc::varArgFixedCount(Name, Variant);
            Fixed && Fixed != Params.size())
          Note("vararg-fixed",
               std::to_string(Fixed) + " vs " + std::to_string(Params.size()));
      // The printf family's formats and the variadic routines' forwards
      // name the fixed parameters too.
      if (const libc::LibCPrintfFormat *Printf = libc::libcPrintfFormat(Name);
          Printf && (!Function.Variadic || Printf->ParamCount != Params.size()))
        Note("printf-format", std::to_string(Printf->ParamCount) + " vs " +
                                  std::to_string(Params.size()) +
                                  (Function.Variadic ? " variadic" : ""));
      if (const libc::LibCVariadicForward *Forward =
              libc::libcVariadicForward(Name);
          Forward && libc::variadicIn(*Forward, Variant)) {
        if (!Function.Variadic)
          Note("forward-not-variadic", std::string(Forward->Target));
        else if (Forward->FixedCount != Params.size())
          Note("forward-fixed", std::to_string(Forward->FixedCount) + " vs " +
                                    std::to_string(Params.size()));
      }
      if (!Function.Variadic && libc::varArgFixedCount(Name, Variant))
        Note("vararg-not-variadic",
             std::to_string(libc::varArgFixedCount(Name, Variant)));
    }
  }
  return Out;
}

} // namespace

TEST(HeaderLibraryCallModel, TheTablesAgreeWithEveryLibrary) {
  // Function discovery, the call analysis and the emitters read the tables
  // where no library joins a program, and before one does: what they say a
  // routine takes and returns is what its C library's headers declare, the
  // routines C libraries declare otherwise by LibCVariants.def.
  const std::vector<Disagreement> Found = disagreements();
  EXPECT_TRUE(Found.empty()) << Found.size() << " disagreements, listed below";
  // Each check and routine once, with the libraries that disagree.
  std::map<std::pair<std::string, std::string>, std::vector<std::string>>
      Grouped;
  for (const Disagreement &D : Found)
    Grouped[{D.Check, D.Routine}].push_back(D.Library + " (" + D.Detail + ")");
  std::map<std::string, unsigned> PerCheck;
  for (const auto &[Key, Libraries] : Grouped) {
    ++PerCheck[Key.first];
    std::cout << Key.first << " " << Key.second << ":";
    for (const std::string &Library : Libraries)
      std::cout << " " << Library;
    std::cout << "\n";
  }
  for (const auto &[Check, Count] : PerCheck)
    std::cout << "TOTAL " << Check << " " << Count << "\n";
}

TEST(HeaderLibraryCallModel, KnowsWhichCLibraryAProgramLinks) {
  EXPECT_EQ(libc::libcVariantOf(BinaryFormat::MachO),
            libc::LibCVariant::Darwin);
  EXPECT_EQ(libc::libcVariantOf(BinaryFormat::COFF),
            libc::LibCVariant::Windows);
  EXPECT_EQ(libc::libcVariantOf(BinaryFormat::ELF), libc::LibCVariant::GNU);
  // Darwin's sendfile is BSD's, and its shm_open is variadic, whose mode
  // Apple's AArch64 convention passes on the stack.
  EXPECT_EQ(libc::libcArity("sendfile", libc::LibCVariant::GNU)->IntArgs, 4);
  EXPECT_EQ(libc::libcArity("sendfile", libc::LibCVariant::Darwin)->IntArgs, 6);
  EXPECT_EQ(libc::varArgFixedCount("shm_open", libc::LibCVariant::Darwin), 2u);
  EXPECT_FALSE(libc::libcArity("shm_open", libc::LibCVariant::Darwin));
  EXPECT_EQ(libc::libcArity("shm_open", libc::LibCVariant::GNU)->IntArgs, 3);
  // The named parameters of a variadic routine are all fixed ones.
  EXPECT_EQ(libc::varArgFixedCount("fcntl", libc::LibCVariant::GNU), 2u);
  EXPECT_EQ(libc::varArgFixedCount("fcntl", libc::LibCVariant::Darwin), 2u);
  EXPECT_EQ(libc::varArgFixedCount("obstack_vprintf", libc::LibCVariant::GNU),
            0u);
  // Darwin's variants of a routine take its arguments; a selector stub's
  // suffix is no variant.
  EXPECT_EQ(
      libc::routineOfSymbol("syslog$DARWIN_EXTSN", libc::LibCVariant::Darwin),
      "syslog");
  EXPECT_EQ(libc::routineOfSymbol("fcntl$NOCANCEL$UNIX2003",
                                  libc::LibCVariant::Darwin),
            "fcntl");
  EXPECT_EQ(
      libc::routineOfSymbol("objc_msgSend$length", libc::LibCVariant::Darwin),
      "objc_msgSend$length");
  EXPECT_EQ(
      libc::routineOfSymbol("syslog$DARWIN_EXTSN", libc::LibCVariant::GNU),
      "syslog$DARWIN_EXTSN");
  EXPECT_EQ(
      libc::varArgFixedCount("syslog$DARWIN_EXTSN", libc::LibCVariant::Darwin),
      2u);
  EXPECT_EQ(libc::varArgFixedCount("objc_msgSend$stringWithFormat:",
                                   libc::LibCVariant::Darwin),
            3u);
}
