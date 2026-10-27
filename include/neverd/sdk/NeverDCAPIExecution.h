//===- NeverDCAPIExecution.h - Persistent workload C API ---------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Persistent workload C API.
///

#ifndef NEVERD_SDK_CAPI_EXECUTION_H
#define NEVERD_SDK_CAPI_EXECUTION_H
#include "neverd/sdk/NeverDCAPIModules.h"

#ifdef __cplusplus
extern "C" {
#endif
/// Opaque, non-reused process-local identity; zero is invalid. Execution and
/// module handles are distinct kinds even though both use uint64 storage.
typedef uint64_t neverd_execution_t;
enum {
#define NEVERD_EXECUTION_INPUT_LIMIT(Name, CName, Value) CName = Value,
#include "neverd/emulation/ExecutionInputLimits.def"
#undef NEVERD_EXECUTION_INPUT_LIMIT
};
/// Exact v1 structure. A valid module_set and non-NULL configuration_json are
/// required. Configuration explicitly selects an environment, run controls
/// and optional bindings; no environment is inferred from a host or filename.
/// The execution retains the source state after its public handle is released.
typedef struct neverd_execution_options_v1 {
  size_t struct_size;
  neverd_module_set_t module_set;
  const char *configuration_json;
} neverd_execution_options_v1;

/// Every function clears optional *Error. Failure returns zero/NULL and an
/// owned UTF-8 error; free errors and JSON with neverd_free_string(). Input and
/// output storage must not overlap. No C++ exception crosses this boundary.
/// JSON inputs are bounded to 2 MiB; execution reports to 192 MiB. Report
/// schema_version is 1; uint64 addresses, counters and tokens are hex strings.
///
/// All operations are synchronous. Except state_json/request_pause, concurrent
/// operations on one execution return busy. Run/source operations also hold
/// the source catalogue's guard, so concurrent additions or another run over
/// that catalogue return busy. Destroy during an active operation returns
/// busy; destroy(0) is a successful no-op. Stale/wrong-kind handles fail.
/// Exports remain available in feature-disabled builds and diagnose unavailable
/// execution. Creation does not run instructions. The buffer environment does
/// not prepare binary resources until they are needed by the execution.
NEVERD_API neverd_execution_t neverd_execution_create_v1(
    const neverd_execution_options_v1 *Options, const char **Error);
NEVERD_API int neverd_execution_destroy(neverd_execution_t Run,
                                        const char **Error);
/// Only state_json and request_pause may be used concurrently with run_json.
/// A pause request is cooperative and does not replenish any quota.
NEVERD_API const char *neverd_execution_state_json(neverd_execution_t Run,
                                                   const char **Error);
NEVERD_API int neverd_execution_request_pause(neverd_execution_t Run,
                                              const char **Error);
/// Run Ready to the next event. A Failed guest outcome is a non-NULL report,
/// distinct from a C API precondition/transport allocation error (NULL).
NEVERD_API const char *neverd_execution_run_json(neverd_execution_t Run,
                                                 const char **Error);
/// Paused -> Ready only. Does not execute. AwaitingResource requires supply.
NEVERD_API int neverd_execution_resume(neverd_execution_t Run,
                                       const char **Error);
NEVERD_API const char *neverd_execution_inspect_json(neverd_execution_t Run,
                                                     const char **Error);
NEVERD_API const char *neverd_execution_instances_json(neverd_execution_t Run,
                                                       const char **Error);
/// One binding object; adding a source alone does not declare/map an instance.
NEVERD_API int neverd_execution_declare_binding_json(neverd_execution_t Run,
                                                     const char *BindingJSON,
                                                     const char **Error);
/// Supply {token,bindings} through the retained request and transaction path.
/// Successful supply makes the execution Ready but does not run it.
NEVERD_API int neverd_execution_supply_json(neverd_execution_t Run,
                                            const char *SupplyJSON,
                                            const char **Error);
/// Install one finite call at an admitted pause stage. run_json drives the
/// same workload; result bytes arrive in its invocation_complete event.
NEVERD_API int neverd_execution_invoke_json(neverd_execution_t Run,
                                            const char *InvocationJSON,
                                            const char **Error);
/// Bounded stopped RAM transfers. The caller owns Bytes. Read copies only on
/// success; a zero length permits NULL. Terminal runs allow read/inspect, but
/// not write/resume/supply/invoke. Guest range/permission errors are explicit.
NEVERD_API int neverd_execution_read_memory(neverd_execution_t Run,
                                            uint64_t Address, uint8_t *Bytes,
                                            size_t Size, const char **Error);
NEVERD_API int neverd_execution_write_memory(neverd_execution_t Run,
                                             uint64_t Address,
                                             const uint8_t *Bytes, size_t Size,
                                             const char **Error);
/// Queries a mapped instance; found=false is a successful query. Address is
/// the bound guest address, not the original image address. Names alone grant
/// neither API behavior nor execution permission.
NEVERD_API const char *
neverd_execution_lookup_symbol_json(neverd_execution_t Run,
                                    const char *InstanceID, const char *Name,
                                    const char **Error);
/// One {id,op,...} command through the same primitive APIs and retained source
/// state. Success returns {schema_version:1,id,ok:true,result}. Errors return
/// NULL plus owned *Error and do not imply execution completed. No implicit
/// retry/resume/run follows an add, supply or invoke. The selected primitive's
/// concurrency contract applies; commands are not a multi-operation
/// transaction. quit acknowledges transport closure only; the caller owns
/// pause/join/destroy.
NEVERD_API const char *neverd_execution_command_json(neverd_execution_t Run,
                                                     const char *CommandJSON,
                                                     const char **Error);
#ifdef __cplusplus
}
#endif
#endif
