//===- TypeJSON.h - Local types as JSON for frontends -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Local types as the JSON the command line, the workbench and scripts read,
/// so that every frontend lays a type out alike.  A record lists its members
/// in order with offsets from the outermost record, anonymous unions and
/// structures nested in place, and the padding the compiler adds as members
/// of their own.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TYPES_TYPEJSON_H
#define NEVERD_TYPES_TYPEJSON_H

#include "neverd/types/LocalTypes.h"
#include "neverd/types/TypeRender.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

#include <optional>

namespace neverd::types {

/// The kind, origin and data model as JSON names them.
llvm::StringRef kindName(TypeKind Kind);
llvm::StringRef originName(TypeOrigin Origin);
llvm::StringRef dataModelName(DataModel Model);
/// The data model \p Name names (LLP64, LP64, ILP32), or none.
std::optional<DataModel> dataModelNamed(llvm::StringRef Name);

/// Every type in brief, in ordinal order:
/// `{"ordinal", "name", "kind", "size", "members", "origin"}`, where size is
/// absent for a type without one and members counts a record's named
/// members, anonymous ones' included.
llvm::json::Array typeListJSON(const TypeLibrary &Library);

/// \p Type in full.  Every type has `ordinal`, `name`, `kind` and `origin`.
/// A record adds `size`, `alignment`, `pack`, `declaredAlignment`,
/// `defined` and `members`.  Each member has `kind` ("field", "bitfield",
/// "struct" or "union" for a record in place, "padding"), `name` (empty
/// when anonymous), `offset` in bytes and `bitOffset` in bits from the
/// outermost record, `size` in bytes, `type` as C spells it in
/// \p Spelling, `refers` (the library type it names through pointers and
/// arrays, if any), a bitfield's `bitWidth` and `storageOffset`, and a
/// record in place's `members`.  An enumeration adds `size`, `underlying`,
/// `flags` and `values` (`name`, `value` as a signed or unsigned integer);
/// a typedef `target`; a function type `declaration`.
llvm::json::Object
typeJSON(const TypeLibrary &Library, const LocalType &Type,
         BuiltinSpelling Spelling = BuiltinSpelling::Windows);

} // namespace neverd::types

#endif // NEVERD_TYPES_TYPEJSON_H
