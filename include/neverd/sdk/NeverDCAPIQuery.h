//===- NeverDCAPIQuery.h - C API image queries and graphs ---------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Read-only interrogation of a loaded image: the info-panel and header
/// tables, control-flow and call graphs, address resolution, byte/string
/// search, and function-level diffing between two sessions.
///
/// All returned strings are heap-allocated via strdup(); callers must
/// free them with neverd_free_string().
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_QUERY_H
#define NEVERD_SDK_CAPI_QUERY_H

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

// ===--------------------------------------------------------------------===//
// Info panels (return JSON)
// ===--------------------------------------------------------------------===//

/// Imports as [{"module","name","ordinal","iat_addr","stubs"}].  "iat_addr"
/// is the format-native data slot; "stubs" lists the executable veneers (ELF
/// PLT entries, Mach-O stubs, import thunks) known to forward to the import,
/// in ascending address order.
NEVERD_API const char *neverd_imports_json(neverd_session_t Sess);
NEVERD_API const char *neverd_exports_json(neverd_session_t Sess);
/// Exact pointer slots that hold an imported symbol's address, from every
/// format's binding metadata (ELF GLOB_DAT and JUMP_SLOT GOT entries, PE IAT
/// entries, Mach-O bindings): [{"addr","name","addend"}] in address order.
/// Slots whose metadata names conflicting symbols are omitted.
NEVERD_API const char *neverd_import_slots_json(neverd_session_t Sess);
/// The unwind record whose code range contains \p Address, as
/// {"begin","end","encoding","personality"?,"language_data"}, or null.
/// "begin" and "end" bound the code the record describes, "encoding" names
/// its format ("dwarf-fde", "x64-unwind-v1", ...), "personality" is the
/// resolved personality routine when one is known, and "language_data" is
/// true when the record carries decoded language-specific data.
NEVERD_API const char *neverd_unwind_frame_json(neverd_session_t Sess,
                                                neverd_va_t Address);
NEVERD_API const char *neverd_segments_json(neverd_session_t Sess);
NEVERD_API const char *neverd_strings_json(neverd_session_t Sess,
                                           int MinLength);
NEVERD_API const char *neverd_xrefs_to_json(neverd_session_t Sess,
                                            neverd_va_t Addr);
NEVERD_API const char *neverd_xrefs_from_json(neverd_session_t Sess,
                                              neverd_va_t Addr);

// ===--------------------------------------------------------------------===//
// Sections / Symbols / Relocations / Headers / Entry points / Dashboard
// ===--------------------------------------------------------------------===//

NEVERD_API const char *neverd_sections_json(neverd_session_t Sess);
NEVERD_API const char *neverd_symbols_json(neverd_session_t Sess);
NEVERD_API const char *neverd_relocs_json(neverd_session_t Sess);
NEVERD_API const char *neverd_headers_json(neverd_session_t Sess);
NEVERD_API const char *neverd_entrypoints_json(neverd_session_t Sess);
NEVERD_API const char *neverd_dashboard_json(neverd_session_t Sess);

// ===--------------------------------------------------------------------===//
// CFG graph (returns JSON: nodes + edges)
// ===--------------------------------------------------------------------===//

NEVERD_API const char *neverd_cfg_json(neverd_session_t Sess,
                                       neverd_va_t FuncEntry);

// ===--------------------------------------------------------------------===//
// Call graph (function-level call relationships)
// ===--------------------------------------------------------------------===//

/// Return an owned JSON object with complete function nodes and call edges.
/// SBF recovery is exact-or-empty under typed host resource budgets: when a
/// budget is exhausted this returns {"nodes":[],"edges":[]} and records the
/// diagnostic for neverd_last_error(). A partial edge relation is never
/// published.
NEVERD_API const char *neverd_callgraph_json(neverd_session_t Sess);

// ===--------------------------------------------------------------------===//
// Address resolution
// ===--------------------------------------------------------------------===//

/// Exact functions include optional display_name/linkage_name, name/display
/// origins, recognition_state and library_annotations. User/stated names keep
/// precedence. This query does not force native analysis; evidence may be
/// pending.
NEVERD_API const char *neverd_resolve_addr(neverd_session_t Sess,
                                           neverd_va_t Addr);

// ===--------------------------------------------------------------------===//
// Byte pattern / string search across all segments
// ===--------------------------------------------------------------------===//

NEVERD_API const char *neverd_search_bytes(neverd_session_t Sess,
                                           const unsigned char *Pattern,
                                           int PatternLen, int MaxResults);
NEVERD_API const char *neverd_search_string(neverd_session_t Sess,
                                            const char *Pattern,
                                            int CaseSensitive, int MaxResults);

// ===--------------------------------------------------------------------===//
// Binary diff (function-level comparison between two sessions)
// ===--------------------------------------------------------------------===//

NEVERD_API const char *neverd_diff_functions(neverd_session_t SessA,
                                             neverd_session_t SessB);
NEVERD_API const char *neverd_diff_decompile(neverd_session_t SessA,
                                             neverd_va_t EntryA,
                                             neverd_session_t SessB,
                                             neverd_va_t EntryB);

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_CAPI_QUERY_H
