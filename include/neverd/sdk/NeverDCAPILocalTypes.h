//===- NeverDCAPILocalTypes.h - C API local types -----------------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The named types of a loaded program, as IDA's Local Types window lists
/// them: read from its PDB when first asked for, laid out with every member's
/// offset, and rendered as C, C++, Rust or Go source.
///
/// All returned strings are heap-allocated via strdup(); callers must
/// free them with neverd_free_string().
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_LOCALTYPES_H
#define NEVERD_SDK_CAPI_LOCALTYPES_H

#include "neverd/sdk/NeverDCAPITypes.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#ifdef NEVERD_EXPORTS
#define NEVERD_API __declspec(dllexport)
#else
#define NEVERD_API __declspec(dllimport)
#endif
#else
#define NEVERD_API __attribute__((visibility("default")))
#endif

/// The program's local types in brief:
/// {"source":"pdb"|"none","dataModel":"LLP64"|"LP64"|"ILP32",
/// "stats":{"structs","unions","enums","declaredOnly","nestingMismatches":
/// [name,...]},"types":[{"ordinal","name","kind","size","members",
/// "origin"},...]}.  "kind" is struct, union, enum, typedef or function;
/// "size" is absent for a type without one, and "members" counts a record's
/// named members, anonymous records' included, or an enumeration's values.
/// An image without a PDB has none.  OptionsJson may be NULL or
/// {"filter":"text","language":"c"|"cpp"|"rust"|"go"}: "filter" keeps the
/// types whose name holds the text, ignoring the case of ASCII letters, and
/// "language" adds "renamed":[[name,spelling],...], the names that language
/// cannot spell as they are.  NULL with neverd_last_error() when no binary
/// is loaded, the options are malformed or the PDB's types cannot be read.
NEVERD_API const char *neverd_local_types_json(neverd_session_t Sess,
                                               const char *OptionsJson);

/// The local type \p Name in full: {"ordinal","name","kind","origin"} and, for
/// a record, "size", "alignment", "pack", "declaredAlignment", "defined" and
/// "members"; for an enumeration "size", "underlying", "flags" and
/// "values":[{"name","value"},...]; for a typedef "target"; for a function
/// type "declaration".  Each member has "kind" (field, bitfield, struct or
/// union for a record in place, padding), "name" (empty when anonymous),
/// "offset" in bytes and "bitOffset" in bits from the outermost record,
/// "size" in bytes, "type" as C spells it, "refers" (the local type it
/// names through pointers and arrays, if any), a bitfield's "bitWidth" and
/// "storageOffset", and a record in place's "members".  OptionsJson may be
/// NULL or {"spelling":"windows"|"ida"|"keywords"|"portable"}, how "type"
/// spells builtins.  NULL with neverd_last_error() when there is no such
/// type or the options are malformed.
NEVERD_API const char *neverd_local_type_json(neverd_session_t Sess,
                                              const char *Name,
                                              const char *OptionsJson);

/// Local types as source.  OptionsJson may be NULL or
/// {"language":"c"|"cpp"|"rust"|"go","spelling":"windows"|"ida"|"keywords"|
/// "portable","offsets":bool,"hexOffsets":bool,"asserts":bool,
/// "types":[name,...],"dependencies":bool}.  Without "types", every type
/// renders as one unit that compiles; with them, those types do, and with
/// "dependencies" (the default) every type they need, else each alone for
/// reading.  "asserts" adds checks of every size and offset that the
/// compiler verifies.  NULL with neverd_last_error() for an unknown type or
/// malformed options.
NEVERD_API const char *neverd_local_types_source(neverd_session_t Sess,
                                                 const char *OptionsJson);

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_CAPI_LOCALTYPES_H
