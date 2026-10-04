//===- NeverDCAPIBytecode.h - External bytecode recovery ---------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_SDK_CAPI_BYTECODE_H
#define NEVERD_SDK_CAPI_BYTECODE_H

#include "neverd/sdk/NeverDCAPITypes.h"

#include <stdint.h>

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

/// Reply exactly once, synchronously, on the decoding thread. The JSON bytes
/// are copied before this function returns and need not be NUL-terminated.
/// Return 1 means accepted, 0 means invalid/repeated/oversized reply. The sink
/// and its context must not escape the decoder callback.
typedef int (*ND_BytecodeInstructionSinkV1)(void *SinkContext, const char *JSON,
                                            size_t Size);

/// A trusted plugin selects an instruction at Address from up to 4096 borrowed
/// bytes, truncated at the function end. Reply with {size,operations} using
/// the existing profile operation/operand grammar, or {error:"diagnostic"}.
/// Match patterns are forbidden for a selected instruction. Bytes, sink and
/// sink context are borrowed only for this invocation. No callback is retained
/// after recovery returns; callbacks are synchronous on the calling thread.
/// The result must depend only on bytes, PC and stable UserData, not CFG visit
/// order. No exception may cross this C boundary.
typedef void (*ND_BytecodeDecoderV1)(void *UserData, const unsigned char *Bytes,
                                     size_t Size, uint64_t Address,
                                     ND_BytecodeInstructionSinkV1 Reply,
                                     void *SinkContext);

/// Recover caller-specified bytecode without loading or executing an image.
/// Code and RequestJSON are borrowed byte buffers for this synchronous call;
/// RequestJSON need not be NUL-terminated. Each input is limited to 64 MiB.
///
/// The strict JSON request contains schemaVersion:1, a version-1 profile object
/// and a functions array. Optional keys: bindings, base, output ("check",
/// "highc" or "llvmc"), optimize (llvmc only), unaligned_pointers,
/// with_context. with_context:true emits uint64_t callee(void *state, void
/// *context) for all source functions and bindings, instead of the default
/// unary state ABI. The source context is caller-owned and separate from
/// decoder UserData. See docs/bytecode-profiles.md for the shared decoder and
/// state-ABI contracts. A plugin can prepare these rules in C or Python; no
/// dialect is built in.
///
/// The owned JSON response contains schemaVersion:1 and ok. Success includes
/// functions, blocks, decoded_instructions, decoded_bytes, input_bytes, scope
/// ("cfg" or "state-c"), with_context (selected ABI, also for check), and
/// source (empty for check). Failure includes error and no partial source. Null
/// means allocation failed. Free every non-null response with
/// neverd_free_string(), including error responses.
///
/// This is source recovery from an explicit semantic specification, not an
/// execution sandbox, proof of interpreter equivalence, or original C ABI
/// reconstruction. Generated memory accesses require caller-valid storage.
NEVERD_API const char *
neverd_bytecode_recover_json_v1(const unsigned char *Code, size_t CodeSize,
                                const char *RequestJSON, size_t RequestSize);

/// Dynamic-decoder variant of the same recovery pipeline. Decoder is required;
/// UserData is borrowed until this call returns. The request has layout instead
/// of profile: version, register_bytes, byte_order, optional temporary_bytes.
/// Encodings are forbidden in layout. All other request, response, ownership,
/// budget and source-ABI rules are identical to the static-profile API.
/// Callback replies have the same 64 MiB bound as other JSON inputs.
/// Unsupported instructions, malformed replies and missing/repeated replies
/// fail the whole recovery without partial source. A callback is trusted host
/// code, not a sandbox or a guest instruction; the engine never executes the
/// input.
NEVERD_API const char *neverd_bytecode_recover_decoder_json_v1(
    const unsigned char *Code, size_t CodeSize, const char *RequestJSON,
    size_t RequestSize, ND_BytecodeDecoderV1 Decoder, void *UserData);

#ifdef __cplusplus
}
#endif
#endif
