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
#define NEVERD_TYPE_LANGUAGE(Name, Id, Display, Spelling) Name,
#include "neverd/types/TypeLanguages.def"
};

/// How APIs name \p Lang, such as "rust".
llvm::StringRef languageId(Language Lang);
/// Every language's id, as a message lists them: "c, cpp, ... or python".
std::string languagesText();
/// The language an API names, if any.
std::optional<Language> languageNamed(llvm::StringRef Id);

/// How rendered source names builtin types (BuiltinSpellings.def).
enum class BuiltinSpelling : uint8_t {
#define NEVERD_BUILTIN_SPELLING(Name, Id) Name,
#include "neverd/types/BuiltinSpellings.def"
};

/// How APIs name \p Spelling, such as "windows".
llvm::StringRef spellingId(BuiltinSpelling Spelling);
/// The spelling an API names, if any.
std::optional<BuiltinSpelling> spellingNamed(llvm::StringRef Id);
/// Whether \p Lang names builtins as \p Spelling.
bool spellsBuiltins(Language Lang, BuiltinSpelling Spelling);
/// How \p Lang names builtins unless asked.
BuiltinSpelling defaultSpelling(Language Lang);
/// The spellings \p Lang takes, by their ids: "windows or portable".
std::string spellingsText(Language Lang);

struct RenderOptions {
  Language Lang = Language::C;
  /// How builtins are named; the language's default when unset, or when
  /// the language does not take it.
  std::optional<BuiltinSpelling> Spelling;
  /// A comment with each member's offset.
  bool Offsets = true;
  /// Offsets in hexadecimal rather than decimal.
  bool HexOffsets = true;
  /// After each record, checks of its size and member offsets that the
  /// compiler verifies.
  bool Asserts = false;
  /// The column offset comments start at.
  unsigned CommentColumn = 64;
  /// When every type renders, the library's function prototypes after them,
  /// in the languages rendersPrototypes says.
  bool Prototypes = false;
};

/// Whether \p Lang renders function prototypes: C and C++ do.
bool rendersPrototypes(Language Lang);
/// Whether \p Lang's declarations read back (TypeParse.h): C, C++, Rust
/// and Go's do; Python and JavaScript render only.
bool readsDeclarations(Language Lang);
/// How the workbench shows \p Lang, such as "Rust".
llvm::StringRef languageDisplay(Language Lang);

/// The spelling \p Options names builtins in.
BuiltinSpelling spellingOf(const RenderOptions &Options);

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

/// The names of the types \p Type holds by value, whose layouts its own
/// depends on: through arrays and records in place, not pointers.
std::vector<std::string> valueDependencyNames(const LocalType &Type);

} // namespace neverd::types

#endif // NEVERD_TYPES_TYPERENDER_H
