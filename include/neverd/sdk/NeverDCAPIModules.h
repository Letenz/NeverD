//===- NeverDCAPIModules.h - Explicit binary resources C API -----*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit binary resources C API.
///

#ifndef NEVERD_SDK_CAPI_MODULES_H
#define NEVERD_SDK_CAPI_MODULES_H
#include "neverd/sdk/NeverDCAPISession.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Opaque, process-local identity; zero is invalid. Never interpret it as an
/// address or serialize it for use in another process. Identities are not
/// reused.
typedef uint64_t neverd_module_set_t;
enum {
#define NEVERD_MODULE_INPUT_LIMIT(Name, CName, Value) CName = Value,
#include "neverd/modules/ModuleInputLimits.def"
#undef NEVERD_MODULE_INPUT_LIMIT
};

/// v1 requires exact struct_size. NULL options creates an empty catalogue.
/// JSON defaults to {}; base_directory is an optional explicit absolute origin
/// and must agree with any base_directory stated in the JSON. Registration
/// performs no binary/debug IO, including for eager inputs. No analysis session
/// or emulation feature is required.
typedef struct neverd_module_set_options_v1 {
  size_t struct_size;
  const char *configuration_json;
  const char *base_directory;
} neverd_module_set_options_v1;

/// Every operation clears *Error when Error is non-NULL. Failure returns zero
/// or NULL and optionally allocates a UTF-8 diagnostic in *Error. Free errors
/// and JSON results with neverd_free_string(). Inputs are borrowed for the
/// call; output error storage must be separate from input objects. Concurrent
/// source operations report busy rather than racing mutable parsing/query
/// caches. Diagnostics are bounded to 64 KiB without splitting UTF-8; JSON
/// results are bounded to 64 MiB.
NEVERD_API neverd_module_set_t neverd_module_set_create_v1(
    const neverd_module_set_options_v1 *Options, const char **Error);
/// Read this explicit configuration file only; its JSON base_directory wins,
/// otherwise input paths use the file's parent directory. Module files remain
/// unprepared. The configuration path is UTF-8 and may be relative.
NEVERD_API neverd_module_set_t neverd_module_set_from_file(const char *Path,
                                                           const char **Error);
/// Release the caller's handle; running consumers may retain its resources.
/// Zero is a successful no-op. Other stale/foreign handles report failure.
NEVERD_API int neverd_module_set_destroy(neverd_module_set_t Set,
                                         const char **Error);
NEVERD_API const char *neverd_module_set_inspect_json(neverd_module_set_t Set,
                                                      const char **Error);
/// ModuleJSON is one module object using the configuration's id/image/debug/
/// load fields. Missing load inherits the catalogue default. No file IO.
NEVERD_API int neverd_module_set_add_json(neverd_module_set_t Set,
                                          const char *ModuleJSON,
                                          const char **Error);
/// Correct a never-prepared declaration using id/image/optional debug. Load
/// must be omitted; the original policy and identity remain fixed. No file IO.
NEVERD_API int neverd_module_set_revise_unprepared_json(neverd_module_set_t Set,
                                                        const char *ModuleJSON,
                                                        const char **Error);
/// Prepare one resource or all eager resources through the common engine.
NEVERD_API int neverd_module_set_prepare(neverd_module_set_t Set,
                                         const char *ID, const char **Error);
NEVERD_API int neverd_module_set_prepare_eager(neverd_module_set_t Set,
                                               const char **Error);
/// DebugJSON is {"kind":"pdb|dwarf|map","path":"..."}. Explicit companion
/// failures never fall back to an adjacent file. Existing observations stay
/// pinned; adding debug information never remaps or resumes an execution.
NEVERD_API int neverd_module_set_attach_debug_json(neverd_module_set_t Set,
                                                   const char *ID,
                                                   const char *DebugJSON,
                                                   const char **Error);
/// Static, original-image symbol address; no guest base or ABI is inferred.
/// Query prepares only the requested resource. A missing symbol is successful
/// JSON with found=false. Addresses/sizes use hexadecimal strings.
NEVERD_API const char *
neverd_module_set_lookup_symbol_json(neverd_module_set_t Set, const char *ID,
                                     const char *Name, const char **Error);

#ifdef __cplusplus
}
#endif
#endif
