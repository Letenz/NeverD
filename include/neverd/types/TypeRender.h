//===- TypeRender.h - Local types as C, C++, Rust and Go --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Local types as source: C and C++ as their headers would declare them,
/// Rust as #[repr(C)] items and Go as structs, every member marked with its
/// offset.  Each language lays the type out exactly as the library records
/// it.  Where its own rules would place a member elsewhere, an explicit
/// filler moves it; a language without unions or bitfields reads them
/// through storage of their size and accessors.
///
/// A type renders alone for reading, or with every type it needs for a
/// header that compiles, optionally followed by checks of each size and
/// offset that the compiler verifies.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TYPES_TYPERENDER_H
#define NEVERD_TYPES_TYPERENDER_H

#include "neverd/types/LocalTypes.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd::types {

/// The languages local types render in (TypeLanguages.def).
enum class Language : uint8_t {
#define NEVERD_TYPE_LANGUAGE(Name, Id, Display) Name,
#include "neverd/types/TypeLanguages.def"
};

/// How APIs name \p Lang, such as "rust".
llvm::StringRef languageId(Language Lang);
/// The language an API names, if any.
std::optional<Language> languageNamed(llvm::StringRef Id);

/// How C and C++ spell builtin types.
enum class BuiltinSpelling : uint8_t {
  Windows,  ///< ULONG and UCHAR, as the Windows headers name them.
  IDA,      ///< unsigned int and unsigned __int8, as IDA's local types do.
  Keywords, ///< unsigned long, as the compiler recorded it.
  Portable, ///< uint32_t and the other exact-width types of <stdint.h>.
};

struct RenderOptions {
  Language Lang = Language::C;
  BuiltinSpelling Spelling = BuiltinSpelling::Windows;
  /// A comment with each member's offset.
  bool Offsets = true;
  /// Offsets in hexadecimal rather than decimal.
  bool HexOffsets = true;
  /// After each record, checks of its size and member offsets that the
  /// compiler verifies.
  bool Asserts = false;
  /// The column offset comments start at.
  unsigned CommentColumn = 64;
};

/// \p Type's declaration, alone.
std::string renderType(const TypeLibrary &Library, const LocalType &Type,
                       const RenderOptions &Options);

/// \p Types and every type they need, in an order that compiles, after what
/// the spelling needs declared.  Every type when \p Types is empty.
std::string renderTypes(const TypeLibrary &Library,
                        llvm::ArrayRef<const LocalType *> Types,
                        const RenderOptions &Options);

/// The names \p Lang cannot spell as they are, such as `std::pair<int,int>`
/// or an enumerator another enumeration has too, each with the identifier
/// rendered source calls it by.  In the library's order.
std::vector<std::pair<std::string, std::string>>
renamedNames(const TypeLibrary &Library, Language Lang);

/// The names of the types \p Type mentions: its members', target's and
/// parameters' types, through pointers, arrays and records in place.
std::vector<std::string> mentionedTypeNames(const LocalType &Type);

} // namespace neverd::types

#endif // NEVERD_TYPES_TYPERENDER_H
