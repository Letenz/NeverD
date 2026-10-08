//===- NeverDCAPISession.h - C API session lifecycle --------------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Creating, loading, and analyzing a session, the image metadata read off it,
/// plus the error, allocation, and version entry points every caller needs.
///
/// All returned strings are heap-allocated via strdup(); callers must
/// free them with neverd_free_string().
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_SESSION_H
#define NEVERD_SDK_CAPI_SESSION_H

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
// Session lifecycle
// ===--------------------------------------------------------------------===//

NEVERD_API neverd_session_t neverd_session_create(void);
NEVERD_API void neverd_session_destroy(neverd_session_t Sess);
/// Returns 1 on success, replacing the image, analysis, and per-image edits.
/// Returns 0 when preparation fails; a previously loaded session remains
/// usable with its existing image, analysis, and edits. See
/// neverd_last_error().
/// Path is UTF-8 on every platform, including Windows.
NEVERD_API int neverd_session_load(neverd_session_t Sess, const char *Path);
NEVERD_API int neverd_session_is_loaded(neverd_session_t Sess);

/// Add the native function entries the function detector finds in the loaded
/// image alone -- call targets, prologues and format tables, without lifting
/// anything -- to the function list.  An added function has an automatic name
/// and no size, and stays listed until the next load; the full pipeline may
/// list more.  Idempotent; returns the function count, or -1 with an error.
NEVERD_API int neverd_session_discover_functions(neverd_session_t Sess);

/// Run the full analysis pipeline (lift → optimize → decompile).
/// Call after neverd_session_load() to pre-compute analysis data.
/// Returns 1 on success, 0 on failure.  Thread-safe if called once.
NEVERD_API int neverd_session_analyze(neverd_session_t Sess);

/// Return the caller's input path encoded as UTF-8.
NEVERD_API const char *neverd_session_file_path(neverd_session_t Sess);
NEVERD_API const char *neverd_session_arch_name(neverd_session_t Sess);
NEVERD_API const char *neverd_session_format_name(neverd_session_t Sess);
NEVERD_API int neverd_session_is_64bit(neverd_session_t Sess);
/// Return the target word size (32, 64, or 256), or 0 when unloaded.
NEVERD_API int neverd_session_bitness(neverd_session_t Sess);

// ===--------------------------------------------------------------------===//
// Debug information
//
// Loading a binary also looks for the debug information that belongs to it —
// PDB for PE, DWARF (in-image, .dSYM, or a split .debug file) for ELF and
// Mach-O, then a linker MAP — and the names it finds populate the symbol table
// the rest of the API reads.  The three setters below adjust that search and
// only take effect on the next neverd_session_load(); the two queries report
// what the last load settled on.
// ===--------------------------------------------------------------------===//

/// Load debug symbols from \p Path instead of searching for a companion file.
/// The named file is authoritative: neverd_session_load() fails if it cannot be
/// read or holds no function symbols.  Pass NULL or "" to resume searching.
/// Path is UTF-8 on every platform.
NEVERD_API void neverd_session_set_pdb_path(neverd_session_t Sess,
                                            const char *Path);

/// Linker MAP counterpart of neverd_session_set_pdb_path().
NEVERD_API void neverd_session_set_map_path(neverd_session_t Sess,
                                            const char *Path);

/// Pass 0 to analyze the image alone, ignoring any debug file beside it.
/// The escape hatch for a stale companion file that names functions worse than
/// the image itself does.  Enabled by default.
NEVERD_API void neverd_session_set_debug_info_enabled(neverd_session_t Sess,
                                                      int Enabled);

/// Limit the next `neverd_session_load()` PE exception-table decode to this
/// function entry.  Catch funclets named by that frame are still decoded.
/// Pass 0 to clear.  Analysis of a different entry after load decodes that
/// entry on demand.  Changing the restriction after an analysis ran discards
/// that analysis, so a later query (including neverd_session_analyze())
/// recomputes it for the new restriction instead of reusing stale results.
NEVERD_API void neverd_session_restrict_function(neverd_session_t Sess,
                                                 neverd_va_t Entry);

/// Assert the instruction state of one AArch32 function entry on subsequent
/// loads. Use this for binaries whose ARM/Thumb mode metadata is missing.
/// The entry is an untagged mapped address (including address zero in an
/// object file). Conflicting symbols, mappings, relocations, CPU constraints,
/// or an undecodable entry make neverd_session_load() fail. Repeating a call
/// replaces that entry's assertion; CLEAR removes it. Returns 0 for an invalid
/// mode or session, otherwise 1. Other architectures reject a nonempty set.
enum neverd_arm_function_mode {
  NEVERD_ARM_FUNCTION_MODE_CLEAR = 0,
  NEVERD_ARM_FUNCTION_MODE_ARM = 1,
  NEVERD_ARM_FUNCTION_MODE_THUMB = 2,
};
NEVERD_API int neverd_session_set_arm_function_mode(neverd_session_t Sess,
                                                    neverd_va_t Entry,
                                                    int Mode);

/// Best-effort exact-name hint for a PE with an explicitly selected PDB.
/// Call after neverd_session_set_pdb_path() and before neverd_session_load().
/// Returns 0 if the name is absent or ambiguous, the files disagree, or the
/// fast lookup is unavailable.  This does not change the session or report an
/// error; callers should load normally and resolve the name in that case.
/// A nonzero result may be passed to neverd_session_restrict_function(), but
/// callers must verify the name after loading and retry without restriction if
/// it no longer resolves to that address.
NEVERD_API neverd_va_t neverd_session_resolve_function_name_before_load(
    neverd_session_t Sess, const char *BinaryPath, const char *Name);

/// Called from `neverd_session_load` on the same thread.  \p phase is a
/// stable token (`image`, `debug`, `ready`).  \p done/\p total are 0 when
/// the step is indeterminate.  \p detail may be empty.  Pass NULL to clear.
typedef void (*neverd_load_progress_fn)(void *user_data, const char *phase,
                                        unsigned long long done,
                                        unsigned long long total,
                                        const char *detail);
NEVERD_API void neverd_session_set_load_progress(neverd_session_t Sess,
                                                 neverd_load_progress_fn Fn,
                                                 void *UserData);

/// Which loader supplied the session's debug symbols: "dwarf", "pdb", "map",
/// or "none".  Caller frees.
NEVERD_API const char *neverd_session_debug_info_kind(neverd_session_t Sess);

/// Path of the file the debug symbols came from, empty when there are none.
/// Caller frees.
NEVERD_API const char *neverd_session_debug_info_path(neverd_session_t Sess);

// ===--------------------------------------------------------------------===//
// Error handling
// ===--------------------------------------------------------------------===//

NEVERD_API const char *neverd_last_error(neverd_session_t Sess);

// ===--------------------------------------------------------------------===//
// Memory management
// ===--------------------------------------------------------------------===//

NEVERD_API void neverd_free_string(const char *Str);

// ===--------------------------------------------------------------------===//
// Session metadata
// ===--------------------------------------------------------------------===//

NEVERD_API unsigned long long neverd_session_file_size(neverd_session_t Sess);
NEVERD_API neverd_va_t neverd_session_base_addr(neverd_session_t Sess);
NEVERD_API neverd_va_t neverd_session_entry_addr(neverd_session_t Sess);
NEVERD_API int neverd_session_segment_count(neverd_session_t Sess);
NEVERD_API int neverd_session_section_count(neverd_session_t Sess);
NEVERD_API int neverd_session_import_count(neverd_session_t Sess);
NEVERD_API int neverd_session_export_count(neverd_session_t Sess);
NEVERD_API int neverd_session_symbol_count(neverd_session_t Sess);

/// \p Size bytes from \p Addr as text, sixteen to a line: the address, the
/// bytes in hexadecimal, and the printable ASCII characters among them.  A
/// byte the image does not map or materialize prints as "??" and a blank,
/// and the lines after the last mapped byte are left out.  NULL when the
/// range maps no byte.
NEVERD_API const char *neverd_hex_dump(neverd_session_t Sess, neverd_va_t Addr,
                                       int Size);
/// neverd_hex_dump() with the text column read in \p TextEncoding, a name or
/// alias of neverd_string_encodings_json(), or ASCII when NULL: a character
/// shows at its first byte in the columns it draws (two for wide East Asian
/// characters), its later bytes filling the rest of its width, and a byte
/// that starts no shown character prints '.'.  Each run of mapped bytes
/// decodes from its first byte.  NULL with an error for an unknown encoding.
NEVERD_API const char *neverd_hex_dump_ex(neverd_session_t Sess,
                                          neverd_va_t Addr, int Size,
                                          const char *TextEncoding);

// ===--------------------------------------------------------------------===//
// Version info
// ===--------------------------------------------------------------------===//

/// Full version string, e.g. "NeverD v3389.0.1".  Caller frees.
NEVERD_API const char *neverd_version(void);

/// Project name only, e.g. "NeverD".  Caller frees.
NEVERD_API const char *neverd_project_name(void);

/// Version number only, e.g. "3389.0.1".  Caller frees.
NEVERD_API const char *neverd_version_number(void);

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_CAPI_SESSION_H
