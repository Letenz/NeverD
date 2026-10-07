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
/// Text strings in the image's initialized data that is not code: C strings
/// in ASCII or UTF-8, UTF-16 or UTF-32 strings in either byte order, and C
/// strings in one legacy code page, each ending in a zero code unit.
/// \p OptionsJson is NULL for the defaults or an object with optional
/// "encodings" (names from neverd_string_encodings_json(), at most one of
/// them a legacy code page; default ascii, utf-8 and utf-16le) and
/// "min_length" (characters, 1 to 1024, default 4).  Returns
/// [{"addr","length","chars","encoding","value"},...] in address order, where
/// "length" counts bytes without the terminator and "value" is UTF-8, or NULL
/// with neverd_last_error() for malformed options.
NEVERD_API const char *neverd_strings_ex_json(neverd_session_t Sess,
                                              const char *OptionsJson);
/// The encodings neverd_strings_ex_json() can search:
/// [{"name","spelling","unit","default","legacy"},...], where "spelling" is
/// how a listing names the encoding (empty for plain ASCII), "unit" the bytes
/// of one code unit, "default" whether a search without options uses it and
/// "legacy" whether it is an 8-bit code page.
NEVERD_API const char *neverd_string_encodings_json(void);
/// The instructions of up to \p MaxFunctions native functions, from
/// \p FirstEntry in entry order, that refer to the text of a string the
/// options (as neverd_strings_ex_json() takes them) find.  A reference into
/// a string reads its text from the first character that starts at or after
/// that byte, and counts only while at least the minimum length of characters
/// remains there.  An instruction that reads or writes a slot the loader
/// relocated, whose pointer reaches such text, refers to it through the slot.
/// Returns {"refs":[["from","to","string",text_offset,"kind",
/// "via"|null],...],"next_entry":"0x..."|null,"function_count":int}: "to"
/// is the referenced byte, "string" the start of the string holding it,
/// text_offset where its text begins in that string's UTF-8 "value", "kind"
/// read, write or offset, and "via" the slot.  Functions decode as for
/// neverd_code_refs_json(), and MaxFunctions is clamped to 1..4096.  NULL
/// with neverd_last_error() for malformed options or an EVM or SBF image.
/// This query never starts analysis.
NEVERD_API const char *neverd_string_refs_json(neverd_session_t Sess,
                                               const char *OptionsJson,
                                               neverd_va_t FirstEntry,
                                               int MaxFunctions);
/// \p Size bytes decoded in \p Encoding (a name or alias of
/// neverd_string_encodings_json()) for display, one cell per byte:
/// {"cells":[...]}, where a character's first byte holds its text (UTF-8), a
/// later byte of it "", and a byte that decodes to nothing shown (a control,
/// an unmapped byte) null.  Wide encodings decode whole units from the first
/// byte.  Returns NULL for an unknown encoding or a size outside 0-65536.
NEVERD_API const char *neverd_decode_text_json(const unsigned char *Bytes,
                                               int Size, const char *Encoding);
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
/// The symbols of neverd_symbols_json that are not functions.
NEVERD_API const char *neverd_data_symbols_json(neverd_session_t Sess);
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
