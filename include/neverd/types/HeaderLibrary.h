//===- HeaderLibrary.h - Type libraries of system headers -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The type libraries NeverD ships (HeaderLibraries.def): the types and
/// prototypes a system's headers declare, which a program's own debug
/// information may lack, as the Windows kernel's PDB lacks every typedef and
/// prototype.  A library joins a program's types where the program has none
/// of the name: the program's own debug information describes its build.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TYPES_HEADERLIBRARY_H
#define NEVERD_TYPES_HEADERLIBRARY_H

#include "neverd/types/LocalTypes.h"

#include "llvm/ADT/StringRef.h"

#include <string>
#include <vector>

namespace neverd::types {

enum class HeaderLibrary : uint8_t {
#define NEVERD_HEADER_LIBRARY(Id, Name, File, Model, Format, Arch, Subsystem)  \
  Id,
#include "neverd/types/HeaderLibraries.def"
};

/// The library's name, as JSON gives it: windows-kernel-x64.
llvm::StringRef headerLibraryName(HeaderLibrary Library);

/// The library's declarations as C, as NeverD embeds them.
llvm::StringRef headerLibrarySource(HeaderLibrary Library);

/// The library read from its declarations the first time it is asked for,
/// every type's origin Header; null with \p Error when they do not read.
/// Safe from any thread.
const TypeLibrary *headerLibrary(HeaderLibrary Library,
                                 std::string *Error = nullptr);

/// What joining a header library to a program's types did.
struct HeaderMerge {
  size_t Types = 0;
  size_t Prototypes = 0;
  /// The header types left out: each holds by value a type whose size the
  /// program gives otherwise, so its layout would not hold, or holds one
  /// left out.
  std::vector<std::string> Conflicts;
};

/// Adds the types and prototypes of \p Headers that \p Library has none of
/// the name of, as HeaderLibrary.h describes.
HeaderMerge mergeHeaderTypes(TypeLibrary &Library, const TypeLibrary &Headers);

} // namespace neverd::types

#endif // NEVERD_TYPES_HEADERLIBRARY_H
