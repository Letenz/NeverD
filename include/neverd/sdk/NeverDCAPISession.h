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
/// List the ways neverd_session_load can read the file at \p Path, as a load
/// dialog shows them, without loading it:
/// {"rows":[{"loader","text","processor","bits","endian","loadable",
/// "by_name","reason"?}]} in list order -- the rows a loader asks to lead,
/// the others, then "Binary file" -- with "processor" as
/// neverd_session_arch_name names it ("" when NeverD has none), "by_name" for
/// a row the loader took for the file's name alone (a dialog should not
/// choose it by default) and "reason" saying why a row cannot be loaded;
/// {"rows":[],"error"} when \p Path names no regular file.  Path is UTF-8.
/// Free with neverd_free_string.
NEVERD_API const char *neverd_identify_json(const char *Path);
/// The PDB the PE at \p Path names, and where it is, without loading the PE:
/// {"pdb":null} for an input that names no PDB a symbol store can key -- not
/// a PE, or one without a unique valid CodeView record -- and otherwise
/// {"pdb":{"file","key","recorded","found":{"path","location"}|null,
/// "rejected":[{"path","location","reason"}],"servers":[{"url","path"}]}}.
/// "file" and "key" name it in a symbol store, as
/// <store>/<file>/<key>/<file>, and "recorded" is the path the linker wrote.
/// "found" is the first local copy whose build identity matches the PE's, in
/// the order neverd_session_load() searches: "recorded" (that path),
/// "beside" (the input's directory), "directory" or "store" (of the search
/// path); "rejected" lists the copies before it that belong to another build
/// or cannot be read.  "servers" are the symbol servers to download a
/// missing PDB from, in order: "url" serves it, and "path" is where its store
/// keeps it, which a later load searches.  \p SymbolPath is a search path in
/// _NT_SYMBOL_PATH syntax; NULL reads _NT_ALT_SYMBOL_PATH and
/// _NT_SYMBOL_PATH, and without either searches a store in the user's cache
/// directory filled from Microsoft's public symbol server.  Nothing is
/// downloaded.  {"pdb":null,"error"} when \p Path names no readable file.
/// Paths are UTF-8.  Free with neverd_free_string.
NEVERD_API const char *neverd_pdb_reference_json(const char *Path,
                                                 const char *SymbolPath);
/// The DWARF of the ELF or Mach-O image at \p Path, found without loading the
/// image: {"dwarf":null} for another format, for an image without DWARF, or
/// for one whose debug information is never read (SBF), and otherwise
/// {"dwarf":{"path","location"}}.  "path" is the file that holds it, and
/// "location" is where neverd_session_load() reads it from, in its order:
/// "input" (the image's own sections), "dsym" (a .dSYM bundle beside a
/// Mach-O image) or "beside" (a split .debug file beside the image, matched
/// by name alone).  {"dwarf":null,"error"} when \p Path names no readable
/// file.  Paths are UTF-8.  Free with neverd_free_string.
NEVERD_API const char *neverd_dwarf_reference_json(const char *Path);
/// Read the input of the next neverd_session_load() as \p OptionsJson says
/// instead of by its header: {"loader":"binary","processor":"x86"|"x86_64"|
/// "arm"|"thumb"|"aarch64","base","offset","size","entry"?,"platform"?}
/// reads it as a binary file -- the bytes from offset on (size 0 for the
/// rest of the file) as that processor's code at base, starting at entry
/// (base when absent); numbers are integers or hexadecimal strings.  The
/// platform whose conventions the code follows decides how it decompiles:
/// "sysv" (System V and AAPCS), "windows" or "darwin"; "auto", the default,
/// reads it from the code -- where calls put their first argument, what
/// prologues preserve, the thread block and system calls the code reaches --
/// and assumes System V when the code shows too little.  {"loader":"evm"}
/// reads a file as EVM bytecode, and {"loader":"auto"} as its header or
/// contents say, even when the input's `.neverd-load.json` says otherwise;
/// NULL drops the request, so a load reads that file when there is one.
/// Returns 0, or -1 with neverd_last_error.
NEVERD_API int neverd_session_set_load_options(neverd_session_t Sess,
                                               const char *OptionsJson);
/// How the loaded image was read, in neverd_session_set_load_options JSON;
/// "{}" before a load.  A binary file's platform comes with
/// "platform_source": "detected" or "user", and with "platform_evidence",
/// what detection read it from.  Free with neverd_free_string.
NEVERD_API const char *neverd_session_load_options_json(neverd_session_t Sess);
/// Keep how the image was read in `<input>.neverd-load.json` for later loads,
/// or remove that file when the header was read.  Returns 0, or -1 with
/// neverd_last_error.
NEVERD_API int neverd_load_options_save(neverd_session_t Sess);
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
/// Return the SHA-256 of the input file's bytes as loaded, as 64 lowercase
/// hexadecimal digits; empty when nothing is loaded or the file cannot be
/// read.  The loader's own hash costs nothing; otherwise the file is hashed
/// once per load.  Free with neverd_free_string.
NEVERD_API const char *neverd_session_input_sha256(neverd_session_t Sess);
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

/// neverd_session_set_pdb_path() with an explicit trust choice for the next
/// load.  With UseWithoutIdentity == 1 the named PDB may supply analysis
/// facts for a PE that records no CodeView identity, as
/// neverd_session_load_pdb_ex() allows for a loaded PE; machine, section
/// layout and any present identity are still checked, and the identity stays
/// unverified.  0 is the strict policy of neverd_session_set_pdb_path().
/// Returns 1, or 0 with neverd_last_error for any other UseWithoutIdentity.
NEVERD_API int neverd_session_set_pdb_path_ex(neverd_session_t Sess,
                                              const char *Path,
                                              int UseWithoutIdentity);

/// Inspect a manually selected PDB for the loaded PE. Returns owned JSON:
/// {"verified":bool,"path","name_count","names":[{"address","name",
/// "function":bool}]}. Names are returned only for an unverified import;
/// authenticated inspection returns their count without a large name payload.
/// A missing CodeView identity permits inspection without authorizing analysis.
/// Present identities,
/// machine and section layout are checked; mismatch or malformed input fails
/// with NULL and neverd_last_error. This does not change the session, function
/// inventory, types, ABI or analysis evidence. Caller frees.
NEVERD_API const char *neverd_session_inspect_pdb_json(neverd_session_t Sess,
                                                       const char *Path);

/// Apply an authenticated PDB to the already loaded PE. Returns 1 on success,
/// keeping user names, comments and edits; 0 with neverd_last_error on failure,
/// leaving the existing image and analysis usable. Missing CodeView identity
/// is an error. The selected path becomes the next-load debug search policy.
NEVERD_API int neverd_session_load_pdb(neverd_session_t Sess, const char *Path);

/// Apply a manually selected PDB with an explicit trust choice. With
/// UseWithoutIdentity == 1, a missing PE CodeView identity is permitted and
/// the PDB supplies the same supported analysis facts as a matching companion.
/// The caller must obtain user confirmation before setting this flag. Its
/// identity remains unverified; machine, sections and present identities are
/// still checked. 0 keeps the strict policy; other values fail. The selection
/// persists in the session's next-load policy. Same transaction and result
/// contract as neverd_session_load_pdb().
NEVERD_API int neverd_session_load_pdb_ex(neverd_session_t Sess,
                                          const char *Path,
                                          int UseWithoutIdentity);

/// Linker MAP counterpart of neverd_session_set_pdb_path().
NEVERD_API void neverd_session_set_map_path(neverd_session_t Sess,
                                            const char *Path);

/// Pass 0 to analyze the image alone, ignoring any debug file beside it.
/// The escape hatch for a stale companion file that names functions worse than
/// the image itself does.  Enabled by default.
NEVERD_API void neverd_session_set_debug_info_enabled(neverd_session_t Sess,
                                                      int Enabled);

/// Pass 0 to load a PE without the PDB it names: the next
/// neverd_session_load() searches for none, though a PDB named with
/// neverd_session_set_pdb_path() still loads and other debug information is
/// still found.  For a user who declined the PDB, or chose one to load after
/// the image.  Enabled by default.
NEVERD_API void neverd_session_set_pdb_search(neverd_session_t Sess,
                                              int Enabled);

/// Pass 0 to load an ELF or Mach-O image without its DWARF, from its own
/// sections or a companion file: the next neverd_session_load() reads none,
/// though other debug information is still found.  For a user who declined
/// it.  Enabled by default.
NEVERD_API void neverd_session_set_dwarf_search(neverd_session_t Sess,
                                                int Enabled);

/// Search the directories and symbol stores of \p SymbolPath, in
/// _NT_SYMBOL_PATH syntax, for the PDB the next neverd_session_load() finds,
/// instead of the environment's; NULL returns to the environment's.  Servers
/// in it are not contacted.  UTF-8.
NEVERD_API void neverd_session_set_symbol_path(neverd_session_t Sess,
                                               const char *SymbolPath);

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

/// Non-fatal loader metadata diagnostics as [{code,message}]. No input bytes
/// are repaired. Returns [] for an unloaded/null session. Caller frees.
NEVERD_API const char *
neverd_session_load_diagnostics_json(neverd_session_t Sess);
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
