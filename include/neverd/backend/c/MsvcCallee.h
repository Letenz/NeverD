//===- MsvcCallee.h - Shared MSVC C-display callees -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Table-driven MSVC display names shared by HighC, LLVMC, and identifier
/// stemming. MsvcCallees.def owns common C++ member rules and includes the
/// library-specific rows from MsvcAtlCallees.def.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_MSVCCALLEE_H
#define NEVERD_BACKEND_C_MSVCCALLEE_H

#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/ir/NdTypes.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace neverd {

enum class MsvcCalleeKind {
#define MSVC_CALLEE(ID, MATCH, DECORATION, MATCH_KIND, ARITY_KIND, MAX_ARGS,   \
                    RETURN_KIND, CLASS_STEM, FASTCALL)                         \
  ID,
#include "neverd/backend/c/MsvcCallees.def"
};

enum class MsvcMatchKind { Exact, Suffix, SuffixOrDtorAlias };
enum class MsvcArityKind { Fixed, CtorDrop, Keep };
enum class MsvcReturnKind { Void, Bool, Int32, VoidPtr, WCharPtr };
enum class MsvcClassStem { FromMatch, CStringT };

struct MsvcCallee {
  MsvcCalleeKind Kind;
  llvm::StringRef Match;
  char Decoration;
  MsvcMatchKind MatchKind;
  MsvcArityKind ArityKind;
  unsigned MaxArgs;
  MsvcReturnKind ReturnKind;
  MsvcClassStem ClassStem;
  bool FastCall;
};

inline llvm::ArrayRef<MsvcCallee> msvcCallees() {
  static const MsvcCallee Table[] = {
#define MSVC_CALLEE(ID, MATCH, DECORATION, MATCH_KIND, ARITY_KIND, MAX_ARGS,   \
                    RETURN_KIND, CLASS_STEM, FASTCALL)                         \
  {MsvcCalleeKind::ID,                                                         \
   MATCH,                                                                      \
   DECORATION,                                                                 \
   MsvcMatchKind::MATCH_KIND,                                                  \
   MsvcArityKind::ARITY_KIND,                                                  \
   MAX_ARGS,                                                                   \
   MsvcReturnKind::RETURN_KIND,                                                \
   MsvcClassStem::CLASS_STEM,                                                  \
   FASTCALL != 0},
#include "neverd/backend/c/MsvcCallees.def"
  };
  return Table;
}

inline bool msvcMatchIdentifier(const MsvcCallee &Row,
                                llvm::StringRef Identifier) {
  switch (Row.MatchKind) {
  case MsvcMatchKind::Exact:
    return Identifier == Row.Match;
  case MsvcMatchKind::Suffix:
  case MsvcMatchKind::SuffixOrDtorAlias:
    if (Row.MatchKind == MsvcMatchKind::SuffixOrDtorAlias &&
        Identifier.contains("::~"))
      return true;
    return Identifier.size() > Row.Match.size() &&
           Identifier.ends_with(Row.Match) &&
           Identifier[Identifier.size() - Row.Match.size() - 1] == '_';
  }
  return false;
}

/// An MSVC-decorated destructor: `??` and the destructor row's decoration code
/// (MsvcCallees.def). That decoration encodes no return type, unlike a
/// constructor or `operator=`, which return `this`.
inline bool isMsvcDestructorName(llvm::StringRef Name) {
  if (!Name.consume_front("??") || Name.empty())
    return false;
  for (const MsvcCallee &Row : msvcCallees())
    if (Row.Kind == MsvcCalleeKind::Dtor)
      return Name.front() == Row.Decoration;
  return false;
}

inline const MsvcCallee *msvcCallee(llvm::StringRef Identifier) {
  for (const MsvcCallee &Row : msvcCallees())
    if (msvcMatchIdentifier(Row, Identifier))
      return &Row;
  return nullptr;
}

/// The MSVC member rules describe the MSVC C++ ABI.  ELF and Mach-O images
/// use Itanium's, whose constructors and operators take other operands, so
/// their stems (`QDomNode_ctor`) match no rule.
inline const MsvcCallee *msvcCallee(llvm::StringRef Identifier,
                                    BinaryFormat Format) {
  if (Format == BinaryFormat::ELF || Format == BinaryFormat::MachO)
    return nullptr;
  return msvcCallee(Identifier);
}

inline const char *msvcSpecialMemberStem(char Decoration) {
  if (!Decoration)
    return nullptr;
  for (const MsvcCallee &Row : msvcCallees())
    if (Row.Decoration == Decoration)
      return Row.Match.data();
  return nullptr;
}

inline TypeRef msvcSyntheticReturn(MsvcReturnKind Kind) {
  switch (Kind) {
  case MsvcReturnKind::Void:
    return NdType::makeVoid();
  case MsvcReturnKind::Bool: {
    auto Ty = NdType::makeInt(1, false);
    Ty->SourceName = "bool";
    return Ty;
  }
  case MsvcReturnKind::Int32:
    return NdType::makeInt(4, true);
  case MsvcReturnKind::VoidPtr:
    return NdType::makePtr();
  case MsvcReturnKind::WCharPtr: {
    auto WChar = NdType::makeInt(2, false);
    WChar->SourceName = "wchar_t";
    return NdType::makePtr(WChar);
  }
  }
  return NdType::makeVoid();
}

inline TypeRef msvcSyntheticThis(llvm::StringRef Identifier,
                                 const MsvcCallee &Row) {
  llvm::StringRef Class;
  if (Row.ClassStem == MsvcClassStem::FromMatch &&
      Identifier.size() > Row.Match.size() + 1 &&
      Identifier.ends_with(Row.Match) &&
      Identifier[Identifier.size() - Row.Match.size() - 1] == '_')
    Class = Identifier.drop_back(Row.Match.size() + 1);
  else
    Class = "CStringT";
  if (Class.empty())
    return NdType::makePtr(NdType::makeInt(1));
  return NdType::makePtr(NdType::makeNamedRecord(Class.str(), 8));
}

template <typename IsUnknown, typename KeepExtra>
size_t msvcPrintedArgLimit(const MsvcCallee &Row, size_t Have,
                           IsUnknown &&UnknownAt, KeepExtra &&Keep) {
  if (Row.ArityKind == MsvcArityKind::Keep)
    return Have;
  if (Row.ArityKind == MsvcArityKind::Fixed)
    return std::min(Have, static_cast<size_t>(Row.MaxArgs));
  size_t Limit = Have;
  while (Limit > Row.MaxArgs && (UnknownAt(Limit - 1) || !Keep(Limit - 1)))
    --Limit;
  const size_t Cap = static_cast<size_t>(Row.MaxArgs) + 2;
  if (Limit > Cap)
    Limit = Cap;
  return Limit;
}

template <typename IsUnknown>
size_t msvcPrintedArgLimit(const MsvcCallee &Row, size_t Have,
                           IsUnknown &&UnknownAt) {
  return msvcPrintedArgLimit(Row, Have, std::forward<IsUnknown>(UnknownAt),
                             [](size_t) { return true; });
}

inline bool msvcTypesCallArgAsPointer(const MsvcCallee &Row, size_t Index) {
  return Index == 0 || (Index == 1 && Row.ArityKind == MsvcArityKind::Fixed &&
                        Row.MaxArgs >= 2);
}

/// Display-only operand type. Concatenate's 3rd/5th are `int32_t` lengths;
/// ABI zext/sext must not print as wrapping casts.
inline TypeRef msvcExpectedCallArgType(const MsvcCallee &Row, size_t Index) {
  if (Row.Kind == MsvcCalleeKind::Concatenate) {
    if (Index == 0)
      return NdType::makePtr(NdType::makeNamedRecord("CStringT", 8));
    if (Index == 1 || Index == 3)
      return msvcSyntheticReturn(MsvcReturnKind::WCharPtr);
    if (Index == 2 || Index == 4)
      return NdType::makeInt(4, true);
    return {};
  }
  if (Row.Kind == MsvcCalleeKind::Format) {
    if (Index == 0)
      return NdType::makePtr(NdType::makeNamedRecord("CStringT", 8));
    if (Index == 1)
      return msvcSyntheticReturn(MsvcReturnKind::WCharPtr);
    // Win64 varargs widen integers to 64 bits. Display them as `int`
    // after peeling ABI zext/cast. Pointer extras still print as the
    // identifier / `&slot` / call (`cstr`), not `(int32_t)p`.
    return NdType::makeInt(4, true);
  }
  if (msvcTypesCallArgAsPointer(Row, Index))
    return NdType::makePtr(NdType::makeInt(1));
  return {};
}

inline std::string msvcSyntheticPrototype(llvm::StringRef Identifier,
                                          const MsvcCallee &Row,
                                          bool FastCall) {
  const TypeRef ReturnType = msvcSyntheticReturn(Row.ReturnKind);
  const TypeRef This = msvcSyntheticThis(Identifier, Row);
  const TypeRef WCharPtr = msvcSyntheticReturn(MsvcReturnKind::WCharPtr);
  std::string Declarator = Identifier.str() + "(";
  if (Row.Kind == MsvcCalleeKind::Format) {
    Declarator += declarationToC(This, "this");
    Declarator += ", ";
    Declarator += declarationToC(WCharPtr, "fmt");
    Declarator += ", ...";
  } else if (Row.Kind == MsvcCalleeKind::Concatenate) {
    Declarator += declarationToC(This, "dest");
    Declarator += ", ";
    Declarator += declarationToC(WCharPtr, "a");
    Declarator += ", ";
    Declarator += declarationToC(NdType::makeInt(4, true), "na");
    Declarator += ", ";
    Declarator += declarationToC(WCharPtr, "b");
    Declarator += ", ";
    Declarator += declarationToC(NdType::makeInt(4, true), "nb");
  } else if (Row.Kind == MsvcCalleeKind::Ctor) {
    // Default / copy / wchar / manager overloads share one C stem.
    Declarator += declarationToC(This, "this");
    Declarator += ", ...";
  } else {
    Declarator += declarationToC(This, "this");
    if (msvcTypesCallArgAsPointer(Row, 1)) {
      Declarator += ", ";
      Declarator += declarationToC(This, "src");
    }
  }
  Declarator += ")";
  std::string Prefix = "extern ";
  if (FastCall)
    Prefix += "__fastcall ";
  return Prefix + declarationToC(ReturnType, Declarator);
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_MSVCCALLEE_H
