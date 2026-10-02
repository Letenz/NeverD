//===- NeverDCAPIBytecode.h - External bytecode recovery ---------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_SDK_CAPI_BYTECODE_H
#define NEVERD_SDK_CAPI_BYTECODE_H

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

/// Recover caller-specified bytecode without loading or executing an image.
/// Code and RequestJSON are borrowed byte buffers for this synchronous call;
/// RequestJSON need not be NUL-terminated. Each input is limited to 64 MiB.
///
/// The strict JSON request contains schemaVersion:1, a version-1 profile object
/// and a functions array. Optional keys: bindings, base, output ("check",
/// "highc" or "llvmc"), optimize (llvmc only), unaligned_pointers.
/// See docs/bytecode-profiles.md for the shared decoder and state-ABI
/// contracts. A plugin can prepare these rules in C or Python; no dialect is
/// built in.
///
/// The owned JSON response contains schemaVersion:1 and ok. Success includes
/// functions, blocks, decoded_instructions, decoded_bytes, input_bytes, scope
/// ("cfg" or "state-c") and source (empty for check). Failure includes error
/// and no partial source. Null means allocation failed. Free every non-null
/// response with neverd_free_string(), including error responses.
///
/// This is source recovery from an explicit semantic specification, not an
/// execution sandbox, proof of interpreter equivalence, or original C ABI
/// reconstruction. Generated memory accesses require caller-valid storage.
NEVERD_API const char *
neverd_bytecode_recover_json_v1(const unsigned char *Code, size_t CodeSize,
                                const char *RequestJSON, size_t RequestSize);

#ifdef __cplusplus
}
#endif
#endif
