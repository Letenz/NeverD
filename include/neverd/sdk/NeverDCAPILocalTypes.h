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
/// An image without a PDB has none.  When a declaration of the user's no
/// longer reads over the program's types, "declarationProblems" lists what
/// it met: [{"declaration" (its index in neverd_type_declarations_json),
/// "line","column","message"},...].  OptionsJson may be NULL or
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

/// Adds the types \p Source declares to the program's local types: C, C++,
/// Rust or Go as OptionsJson's "language" says ("c" when NULL), which may
/// replace types of their names.  Returns {"types":[name,...],
/// "prototypes":[name,...],"problems":[{"line","column","message"},...]}:
/// with any problem, nothing changes.  The declaration joins the user's,
/// which are read again over the program's types whenever those are read
/// anew, as after loading a PDB; neverd_type_declarations_save keeps them.
/// NULL with neverd_last_error() when no binary is loaded or the options
/// are malformed.
NEVERD_API const char *neverd_local_types_declare(neverd_session_t Sess,
                                                  const char *Source,
                                                  const char *OptionsJson);
/// Removes the types the JSON array \p NamesJson names from the program's
/// local types, the program's own as well as the user's.  Returns
/// {"removed":[name,...],"usedBy":[name,...]}: the types that still name a
/// removed one keep their layout and name it as a type the program lacks.
/// The removal joins the user's declarations.  NULL with neverd_last_error()
/// when no binary is loaded, the names are malformed, or one names no type.
NEVERD_API const char *neverd_local_types_forget(neverd_session_t Sess,
                                                 const char *NamesJson);
/// The user's declarations in the order made:
/// [{"language","source"} | {"forget":[name,...]}, ...].
NEVERD_API const char *neverd_type_declarations_json(neverd_session_t Sess);
/// Write or read the declarations as `<input>.neverd-types.json`, which
/// neverd_session_load reads too.  Reading them reads the program's types
/// anew.  Return 0, or -1 with neverd_last_error.
NEVERD_API int neverd_type_declarations_save(neverd_session_t Sess);
NEVERD_API int neverd_type_declarations_load(neverd_session_t Sess);

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_CAPI_LOCALTYPES_H
