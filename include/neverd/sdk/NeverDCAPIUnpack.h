//===- NeverDCAPIUnpack.h - Packed executable recovery -----------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_SDK_CAPI_UNPACK_H
#define NEVERD_SDK_CAPI_UNPACK_H
#include "neverd/sdk/NeverDCAPISession.h"

#ifdef __cplusplus
extern "C" {
#endif
enum {
#define NEVERD_UNPACK_REPORT_LIMIT(Name, CName, Value) CName = Value,
#include "neverd/unpack/Unpack.def"
#undef NEVERD_UNPACK_REPORT_LIMIT
};

/// Recover the image that the packed PE32+ executable InputPath builds at run
/// time and write it to OutputPath. See docs/unpack.md. Both paths are
/// required, nonempty and distinct. The session's loaded analysis image is
/// neither required nor changed. Requires CPU or driver emulation enabled.
///
/// The input runs as a bounded windows-pe64-v1 guest process whose unmodeled
/// loader facts are deferred, and is rebuilt at the transfer into generated
/// code that is accepted as its entry: by default the first one made on the
/// stack the process started with. OptionsJSON is NULL for defaults, or a
/// NUL-terminated UTF-8 JSON object of at most
/// NEVERD_UNPACK_OPTIONS_JSON_LIMIT bytes. It accepts every key of
/// neverd_emulate_process_json with the same meaning, plus "transfer": the
/// one-based transfer to accept instead, and "snapshot_only": an explicit
/// request for image bytes without reconstructing external runtime state.
/// Unknown fields, invalid values and
/// unavailable backends fail before entry.
///
/// The owned JSON report names the identified packer and its evidence, the
/// outcome, every observed transfer, the recovered entry point, sections,
/// imports and the bounded run. outcome "unpacked" means OutputPath was
/// written; "snapshot" means analysis bytes were explicitly requested and
/// written. "no_entry" means the run ended first. "unsupported_state" means
/// an accepted image has possible unreconstructed heap dependencies or lacks
/// heap inventory provenance. Neither writes or truncates OutputPath. The
/// runtime_state report counts conservative matches; no pointer type or
/// relocation is inferred. Addresses are hexadecimal strings. Release with
/// neverd_free_string(). NULL means setup/API failure; inspect
/// neverd_last_error(Sess).
NEVERD_API const char *neverd_unpack_json(neverd_session_t Sess,
                                          const char *InputPath,
                                          const char *OutputPath,
                                          const char *OptionsJSON);
#ifdef __cplusplus
}
#endif
#endif
