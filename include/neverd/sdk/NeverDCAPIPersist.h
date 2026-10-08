//===- NeverDCAPIPersist.h - C API persisted user edits -----------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Per-address annotations and function renames, each persisted to a JSON
/// sidecar file next to the analyzed binary.
///
/// All returned strings are heap-allocated via strdup(); callers must
/// free them with neverd_free_string().
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_PERSIST_H
#define NEVERD_SDK_CAPI_PERSIST_H

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
// Annotations (per-address user comments, persisted to JSON sidecar file)
// ===--------------------------------------------------------------------===//

NEVERD_API void neverd_annotation_set(neverd_session_t Sess, neverd_va_t Addr,
                                      const char *Text);
NEVERD_API void neverd_annotation_remove(neverd_session_t Sess,
                                         neverd_va_t Addr);
NEVERD_API const char *neverd_annotation_get(neverd_session_t Sess,
                                             neverd_va_t Addr);
NEVERD_API const char *neverd_annotations_json(neverd_session_t Sess);
NEVERD_API int neverd_annotations_save(neverd_session_t Sess);
NEVERD_API int neverd_annotations_load(neverd_session_t Sess);

// ===--------------------------------------------------------------------===//
// Symbol renaming (persisted to JSON sidecar file)
// ===--------------------------------------------------------------------===//

NEVERD_API int neverd_rename_func(neverd_session_t Sess, const char *OldName,
                                  const char *NewName);
NEVERD_API const char *neverd_renames_json(neverd_session_t Sess);
NEVERD_API int neverd_renames_save(neverd_session_t Sess);
NEVERD_API int neverd_renames_load(neverd_session_t Sess);

/// Make \p Entry, an address in executable code where no function starts or
/// one neverd_func_delete removed, the entry of a function.  The user's last
/// edit at an address decides over the image, the detector and analysis.
/// Returns 0, or -1 with neverd_last_error.  Analysis restarts with the new
/// function list; neverd_functions_save keeps the edit.
NEVERD_API int neverd_func_create(neverd_session_t Sess, neverd_va_t Entry);
/// Stop treating the function entry \p Entry as one, whether the image,
/// analysis or neverd_func_create made it; no source makes it one again.
/// Returns 0, or -1 with neverd_last_error.
NEVERD_API int neverd_func_delete(neverd_session_t Sess, neverd_va_t Entry);
/// The user's function edits, [{"addr","state"}] in address order, where
/// "state" is "created" or "deleted".  Free with neverd_free_string.
NEVERD_API const char *neverd_functions_json(neverd_session_t Sess);
/// Write or read the edits as `<input>.neverd-functions.json`, which
/// neverd_session_load reads too.  Return 0, or -1 with neverd_last_error.
NEVERD_API int neverd_functions_save(neverd_session_t Sess);
NEVERD_API int neverd_functions_load(neverd_session_t Sess);

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_CAPI_PERSIST_H
