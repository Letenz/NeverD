//===- CIdentifier.h - Deterministic C identifier allocation ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal helpers shared by the HighIR and LLVM-IR C projections.  Binary
/// symbol tables and LLVM quoted names are byte strings, not C identifiers;
/// this allocator converts them without ever copying an uncontrolled byte into
/// active source and resolves collisions in stable encounter order.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_CIDENTIFIER_H
#define NEVERD_LIB_BACKEND_C_CIDENTIFIER_H

#include "neverd/backend/c/MsvcCallee.h"
#include "neverd/loader/SymbolDecoration.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Demangle/Demangle.h"

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace neverd {

inline bool isCProjectionKeyword(llvm::StringRef Name) {
  static constexpr llvm::StringLiteral Keywords[] = {
      "_Alignas",
      "_Alignof",
      "_Atomic",
      "_BitInt",
      "_Bool",
      "_Complex",
      "_Decimal128",
      "_Decimal32",
      "_Decimal64",
      "_Generic",
      "_Imaginary",
      "_Noreturn",
      "_Static_assert",
      "_Thread_local",
      "alignas",
      "alignof",
      "asm",
      "auto",
      "bool",
      "break",
      "case",
      "char",
      "const",
      "constexpr",
      "continue",
      "default",
      "do",
      "double",
      "else",
      "enum",
      "extern",
      "false",
      "float",
      "for",
      "goto",
      "if",
      "inline",
      "int",
      "long",
      "nullptr",
      "register",
      "restrict",
      "return",
      "short",
      "signed",
      "sizeof",
      "static",
      "static_assert",
      "struct",
      "switch",
      "thread_local",
      "true",
      "typedef",
      "typeof",
      "typeof_unqual",
      "union",
      "unsigned",
      "void",
      "volatile",
      "while",
  };
  for (llvm::StringRef Keyword : Keywords)
    if (Name == Keyword)
      return true;
  return false;
}

inline bool isCProjectionIdentifierByte(unsigned char Ch) {
  return (Ch >= 'a' && Ch <= 'z') || (Ch >= 'A' && Ch <= 'Z') ||
         (Ch >= '0' && Ch <= '9') || Ch == '_';
}

inline llvm::StringRef stripImportSymbolPrefix(llvm::StringRef Raw) {
  while (Raw.consume_front("__imp_") || Raw.consume_front("_imp_"))
    ;
  return Raw;
}

/// `??0?$CStringT@...` / `??1?$Foo@H@@...` / `??4?$CStringT@...` /
/// `??B?$CSimpleStringT@...` → `CStringT_ctor` / `Foo_dtor` /
/// `CStringT_assign` / `CSimpleStringT_cstr`.
/// Non-template `??1Widget@@` is `Widget_dtor` via \ref msvcDecorationStem.
inline std::string msvcTemplateSpecialMemberStem(llvm::StringRef Raw) {
  Raw = stripImportSymbolPrefix(Raw);
  if (!Raw.consume_front("??") || Raw.empty())
    return {};
  const char *Suffix = msvcSpecialMemberStem(Raw.front());
  if (!Suffix)
    return {};
  Raw = Raw.drop_front();
  if (!Raw.consume_front("?$"))
    return {};
  const size_t At = Raw.find('@');
  if (At == llvm::StringRef::npos || At == 0)
    return {};
  const llvm::StringRef Name = Raw.take_front(At);
  if (Name.empty() ||
      (!isCProjectionIdentifierByte(static_cast<unsigned char>(Name.front())) &&
       Name.front() != '_'))
    return {};
  for (unsigned char Ch : Name.bytes())
    if (!isCProjectionIdentifierByte(Ch))
      return {};
  return (Name + "_" + Suffix).str();
}

/// `??$LookupText@V?$CStringT@...` → `LookupText`.
inline std::string msvcTemplateFunctionStem(llvm::StringRef Raw) {
  Raw = stripImportSymbolPrefix(Raw);
  if (!Raw.consume_front("??$"))
    return {};
  const size_t At = Raw.find('@');
  if (At == llvm::StringRef::npos || At == 0)
    return {};
  const llvm::StringRef Name = Raw.take_front(At);
  if (Name.empty() ||
      (!isCProjectionIdentifierByte(static_cast<unsigned char>(Name.front())) &&
       Name.front() != '_'))
    return {};
  for (unsigned char Ch : Name.bytes())
    if (!isCProjectionIdentifierByte(Ch))
      return {};
  return Name.str();
}

/// MSVC `?Name@Class@Namespace@@...` → `Namespace_Class_Name`.  Hex-escaping
/// `?` as `_x3F_` made every C++ callee look like a raw decoration next to
/// Hex-Rays' demangled spelling.
inline std::string msvcDecorationStem(llvm::StringRef Raw) {
  if (!Raw.starts_with("?"))
    return {};
  Raw = Raw.drop_front();
  // Only `??` introduces an operator or special member code; after a single
  // `?` the name itself starts, so `?BTreeSearch` is not `operator` +
  // `TreeSearch`.
  bool SpecialMember = false;
  while (!Raw.empty() && Raw.front() == '?') {
    Raw = Raw.drop_front();
    SpecialMember = true;
  }
  const char *Suffix = nullptr;
  if (SpecialMember && !Raw.empty()) {
    Suffix = msvcSpecialMemberStem(Raw.front());
    if (Suffix)
      Raw = Raw.drop_front();
  }
  std::vector<llvm::StringRef> Parts;
  while (!Raw.empty()) {
    const size_t At = Raw.find('@');
    const llvm::StringRef Part =
        At == llvm::StringRef::npos ? Raw : Raw.take_front(At);
    if (Part.empty())
      break;
    const unsigned char Front = static_cast<unsigned char>(Part.front());
    if ((Front >= '0' && Front <= '9') ||
        (Front != '_' && !isCProjectionIdentifierByte(Front)))
      break;
    bool Ident = true;
    for (unsigned char Ch : Part.bytes()) {
      if (!isCProjectionIdentifierByte(Ch)) {
        Ident = false;
        break;
      }
    }
    if (!Ident)
      break;
    Parts.push_back(Part);
    if (At == llvm::StringRef::npos)
      break;
    Raw = Raw.drop_front(At + 1);
  }
  if (Parts.empty())
    return {};
  std::string Out;
  for (size_t I = Parts.size(); I > 0; --I) {
    if (!Out.empty())
      Out += '_';
    Out += Parts[I - 1];
  }
  if (Suffix) {
    Out += '_';
    Out += Suffix;
  }
  return Out;
}

/// Demangled C++ `Ns::Class::Method` / `` `anonymous namespace'::Foo `` →
/// a C identifier (`Ns_Class_Method`, `Foo`).  Templates are cut at `<`.
inline std::string cxxDropAngleArgs(llvm::StringRef Raw) {
  std::string Out;
  unsigned Depth = 0;
  for (unsigned char Ch : Raw.bytes()) {
    if (Ch == '<') {
      ++Depth;
      continue;
    }
    if (Ch == '>') {
      if (Depth)
        --Depth;
      continue;
    }
    if (Depth)
      continue;
    Out.push_back(static_cast<char>(Ch));
  }
  return Out;
}

inline std::string cxxQualifiedStem(llvm::StringRef Raw) {
  while (!Raw.empty()) {
    if (Raw.consume_front("`anonymous namespace'::") ||
        Raw.consume_front("<anonymous namespace>::") ||
        Raw.consume_front("(anonymous namespace)::") ||
        Raw.consume_front("`anonymous-namespace'::"))
      continue;
    break;
  }
  const bool IsDtor = Raw.contains("::~") || Raw.starts_with("~");
  const std::string Stripped = cxxDropAngleArgs(Raw);
  Raw = Stripped;
  if (IsDtor && !Raw.contains("::")) {
    if (Raw.consume_front("~") && !Raw.empty())
      return (Raw + "_dtor").str();
    return {};
  }
  if (!Raw.contains("::"))
    return {};
  std::vector<llvm::StringRef> Parts;
  llvm::StringRef Rest = Raw;
  while (!Rest.empty()) {
    const size_t Sep = Rest.find("::");
    llvm::StringRef Part =
        Sep == llvm::StringRef::npos ? Rest : Rest.take_front(Sep);
    if (Part.consume_front("~"))
      ;
    if (Part.empty() || (!isCProjectionIdentifierByte(
                             static_cast<unsigned char>(Part.front())) &&
                         Part.front() != '_'))
      return {};
    for (unsigned char Ch : Part.bytes()) {
      if (!isCProjectionIdentifierByte(Ch))
        return {};
    }
    Parts.push_back(Part);
    if (Sep == llvm::StringRef::npos)
      break;
    Rest = Rest.drop_front(Sep + 2);
  }
  if (Parts.empty())
    return {};
  const bool IsCtor =
      !IsDtor && Parts.size() >= 2 && Parts.back() == Parts[Parts.size() - 2];
  if ((IsDtor || IsCtor) && Parts.size() >= 2 &&
      Parts.back() == Parts[Parts.size() - 2])
    Parts.pop_back();
  std::string Out;
  for (llvm::StringRef Part : Parts) {
    if (!Out.empty())
      Out += '_';
    Out += Part;
  }
  if (IsDtor)
    Out += "_dtor";
  else if (IsCtor)
    Out += "_ctor";
  return Out;
}

/// The C identifier stem of C++ operator function \p Base: `operator==` is
/// `equal` and `operator bool` is `to_bool` (CxxOperatorNames.def); empty
/// for other names.
inline std::string cxxOperatorStem(llvm::StringRef Base) {
  if (!Base.consume_front("operator") ||
      (!Base.empty() &&
       isCProjectionIdentifierByte(static_cast<unsigned char>(Base.front()))))
    return {};
  Base = Base.trim();
#define NEVERD_CXX_OPERATOR(Spelling, Stem)                                    \
  if (Base == Spelling)                                                        \
    return Stem;
#include "neverd/backend/c/CxxOperatorNames.def"
  // A conversion operator names its type, which starts with a letter.
  if (Base.empty() || !(llvm::isAlpha(Base.front()) || Base.front() == '_'))
    return {};
  std::string Out = "to";
  bool Separate = true;
  for (unsigned char Ch : cxxDropAngleArgs(Base)) {
    if (!isCProjectionIdentifierByte(Ch)) {
      Separate = true;
      continue;
    }
    if (Separate)
      Out += '_';
    Out += static_cast<char>(Ch);
    Separate = false;
  }
  return Out;
}

/// The C identifier stem of the function Itanium C++ symbol \p Raw mangles:
/// its scopes and name joined by `_` as for MSVC names
/// (`_ZNK8QDomNode8nodeTypeEv` is `QDomNode_nodeType`, a constructor
/// `QDomNode_ctor`); empty for other names.
inline std::string itaniumStem(llvm::StringRef Raw) {
  if (!Raw.starts_with("_Z"))
    return {};
  const std::string Mangled = Raw.str();
  llvm::ItaniumPartialDemangler Demangler;
  if (Demangler.partialDemangle(Mangled.c_str()) || !Demangler.isFunction())
    return {};
  auto Take = [](char *Buffer) {
    std::string Text = Buffer ? Buffer : "";
    std::free(Buffer);
    return Text;
  };
  size_t Size = 0;
  const std::string Context =
      Take(Demangler.getFunctionDeclContextName(nullptr, &Size));
  Size = 0;
  const std::string Base = Take(Demangler.getFunctionBaseName(nullptr, &Size));
  if (Base.empty())
    return {};
  std::string Name = cxxOperatorStem(Base);
  if (!Name.empty() && Context.empty())
    Name = "operator_" + Name;
  else if (Name.empty())
    Name = cxxDropAngleArgs(Base);
  if (Context.empty()) {
    for (unsigned char Ch : Name)
      if (!isCProjectionIdentifierByte(Ch))
        return {};
    return Name;
  }
  return cxxQualifiedStem(Context + "::" + Name);
}

/// The demangled spelling of mangled C name \p Name for a comment above its
/// definition (`QDomNode::nodeType() const`), with `*/` broken apart; empty
/// when \p Name is not mangled.
inline std::string demangledComment(llvm::StringRef Name) {
  std::string Demangled = llvm::demangle(Name);
  if (Demangled == Name)
    return {};
  for (size_t At = Demangled.find("*/"); At != std::string::npos;
       At = Demangled.find("*/", At + 2))
    Demangled.replace(At, 2, "* /");
  return Demangled;
}

/// \p Name, without one leading underscore where the C runtime's start files
/// define the same name for \p Format (CRuntimeDefinitions.def): the C
/// identifier a recovered definition of it takes.
inline llvm::StringRef cDefinitionName(llvm::StringRef Name,
                                       BinaryFormat Format) {
#define NEVERD_C_RUNTIME_DEFINITION(FormatId, RuntimeName)                     \
  if (Format == BinaryFormat::FormatId && Name == RuntimeName)                 \
    return Name.drop_front();
#include "neverd/backend/c/CRuntimeDefinitions.def"
  return Name;
}

inline std::string
canonicalizeCProjectionIdentifier(llvm::StringRef Raw,
                                  llvm::StringRef Fallback = "nd_symbol") {
  Raw = stripImportSymbolPrefix(Raw);
  const std::string TemplateMember = msvcTemplateSpecialMemberStem(Raw);
  const std::string TemplateFunction = msvcTemplateFunctionStem(Raw);
  const std::string Decorated = msvcDecorationStem(Raw);
  const std::string Qualified = cxxQualifiedStem(Raw);
  const std::string Itanium = itaniumStem(Raw);
  std::string Stripped;
  if (!TemplateMember.empty()) {
    Raw = TemplateMember;
  } else if (!TemplateFunction.empty()) {
    Raw = TemplateFunction;
  } else if (!Decorated.empty()) {
    Raw = Decorated;
  } else if (!Qualified.empty()) {
    Raw = Qualified;
  } else if (!Itanium.empty()) {
    Raw = Itanium;
  } else {
    llvm::StringRef Rest = Raw;
    while (Rest.consume_front("`anonymous namespace'::") ||
           Rest.consume_front("<anonymous namespace>::") ||
           Rest.consume_front("(anonymous namespace)::") ||
           Rest.consume_front("`anonymous-namespace'::"))
      ;
    if (const size_t Lt = Rest.find('<'); Lt != llvm::StringRef::npos)
      Rest = Rest.take_front(Lt);
    Stripped = Rest.str();
    Raw = Stripped;
  }
  std::string Result;
  Result.reserve(Raw.size());
  for (unsigned char Ch : Raw.bytes()) {
    if (isCProjectionIdentifierByte(Ch)) {
      Result.push_back(static_cast<char>(Ch));
      continue;
    }
    Result += "_x";
    Result.push_back(llvm::hexdigit(Ch >> 4, /*LowerCase=*/false));
    Result.push_back(llvm::hexdigit(Ch & 0x0f, /*LowerCase=*/false));
    Result.push_back('_');
  }

  if (Result.empty())
    Result = Fallback.str();
  if (Result.empty())
    Result = "nd_symbol";
  if (Result.front() >= '0' && Result.front() <= '9')
    Result.insert(0, "nd_");

  // Keywords cannot be identifiers.  Leading underscores are kept: CRT names
  // (`__security_check_cookie`) are what the image already uses, and prefixing
  // `nd_` made every Windows runtime symbol look synthetic next to Hex-Rays.
  if (isCProjectionKeyword(Result))
    Result.insert(0, "nd_");
  return Result;
}

/// C++ TPI spellings (`ATL::CStringT<wchar_t, ...>`) become a C tag
/// (`CStringT`). Nested members after a template (`ATL::CAtlMap<...>::CNode`)
/// keep `CNode`; stripping at the first `<` used to leave `CAtlMap`.
inline std::string cNamedTypeSpelling(llvm::StringRef Raw) {
  std::string Buf;
  Buf.reserve(Raw.size());
  unsigned Depth = 0;
  for (unsigned char Ch : Raw.bytes()) {
    if (Ch == '<') {
      ++Depth;
      continue;
    }
    if (Ch == '>') {
      if (Depth)
        --Depth;
      continue;
    }
    if (Depth == 0)
      Buf.push_back(static_cast<char>(Ch));
  }
  llvm::StringRef Stripped = Buf;
  while (Stripped.starts_with("::"))
    Stripped = Stripped.drop_front(2);
  while (Stripped.ends_with("::"))
    Stripped = Stripped.drop_back(2);
  if (const size_t Sep = Stripped.rfind("::"); Sep != llvm::StringRef::npos)
    Stripped = Stripped.drop_front(Sep + 2);
  return canonicalizeCProjectionIdentifier(Stripped, "nd_type");
}

/// Unnamed image data in C is named as the listing names it
/// (neverd/DataNames.def), so the reader finds the disassembly operand's name
/// in the C: `off_3FC0` for a slot that holds a pointer or that the code calls
/// through, the size of its accesses otherwise (`qword_3FB8`), and `unk_` for
/// data reached by its address alone.
#define NEVERD_DATA_ITEM_NAME(Id, Prefix)                                      \
  inline constexpr llvm::StringLiteral k##Id(Prefix);
#include "neverd/DataNames.def"

/// The automatic name prefix of data accessed \p Bytes at a time; empty when
/// no access size names it.
inline llvm::StringRef dataSizeNamePrefix(uint64_t Bytes) {
#define NEVERD_DATA_SIZE_NAME(SizeKeyword, Size, Prefix)                       \
  if (Bytes == Size)                                                           \
    return Prefix;
#include "neverd/DataNames.def"
  return {};
}

/// The automatic name of the data at \p Addr: one name for it whether the
/// code reads it as a pointer slot (\p PointerSlot), through accesses of the
/// one size \p AccessBytes, or by address alone.
inline std::string makeDataName(uint64_t Addr, bool PointerSlot,
                                std::optional<uint64_t> AccessBytes) {
  llvm::StringRef Prefix = kUnknownNamePrefix;
  if (PointerSlot)
    Prefix = kPointerNamePrefix;
  else if (AccessBytes)
    if (const llvm::StringRef Sized = dataSizeNamePrefix(*AccessBytes);
        !Sized.empty())
      Prefix = Sized;
  return (Prefix + llvm::utohexstr(Addr)).str();
}

/// The byte array that projects overlapping image objects in HighC:
/// `g_<hex VA>_bytes`.  HighC emits a tentative definition so standalone C
/// can link; LLVMC emits `extern` for LLVM `external global`.
inline constexpr llvm::StringLiteral kSyntheticGlobalPrefix("g_");

inline std::string makeSyntheticGlobalName(uint64_t Addr) {
  return (kSyntheticGlobalPrefix + llvm::utohexstr(Addr)).str();
}

class CProjectionIdentifierAllocator {
public:
  std::string allocate(llvm::StringRef Raw,
                       llvm::StringRef Fallback = "nd_symbol") {
    const std::string Base = canonicalizeCProjectionIdentifier(Raw, Fallback);
    std::string Candidate = Base;
    unsigned Suffix = 2;
    while (!Used.insert(Candidate).second)
      Candidate = Base + "_" + std::to_string(Suffix++);
    return Candidate;
  }

private:
  std::set<std::string> Used;
};

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_CIDENTIFIER_H
