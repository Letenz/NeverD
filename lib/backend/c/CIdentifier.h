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

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <set>
#include <string>

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

inline std::string
canonicalizeCProjectionIdentifier(llvm::StringRef Raw,
                                  llvm::StringRef Fallback = "nd_symbol") {
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

  // Avoid every reserved spelling at file scope, not just language keywords.
  if (isCProjectionKeyword(Result) || Result.front() == '_')
    Result.insert(0, "nd_");
  return Result;
}

/// Unnamed image data in C: `g_<hex VA>`.  The C type is already in the
/// declaration (`int32_t g_1400050E0`), so IDA listing dummy names
/// (`byte_`/`word_`/`dword_`/`qword_`) would only repeat width and go stale
/// if the object is later a struct, an array, or a different access size.
/// HighC emits a tentative definition so standalone C can link; LLVMC emits
/// `extern` for LLVM `external global`.
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
