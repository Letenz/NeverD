//===- NeverDCAPIDevirtualize.h - Interpreter recovery C API ----*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_SDK_CAPI_DEVIRTUALIZE_H
#define NEVERD_SDK_CAPI_DEVIRTUALIZE_H

#include "neverd/sdk/NeverDCAPITypes.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Context hint relative to the entry value of RSP. It never assumes the slot
/// is initialized or non-aliasing. bytes must be in [1,8].
typedef struct neverd_devirtualize_frame_slot_v1 {
  int64_t offset;
  uint16_t bytes;
  uint16_t reserved;
} neverd_devirtualize_frame_slot_v1;

/// Experimental, bounded x64 interpreter specialization. Control registers
/// select context separation; they never supply concrete entry input values.
/// Use full x64 GPR names, e.g. "r10". Null options select default budgets.
/// struct_size must cover this complete v1 structure; future tails are ignored.
typedef struct neverd_devirtualize_options_v1 {
  size_t struct_size;
  const char *const *control_registers;
  size_t control_register_count;
  const neverd_devirtualize_frame_slot_v1 *control_frame_slots;
  size_t control_frame_slot_count;
  uint32_t max_nodes;
  uint32_t max_contexts_per_address;
  uint64_t max_operations;
  int use_llvm;
  int no_opt;
  uint32_t reserved;
} neverd_devirtualize_options_v1;

/// Return recovered C only when all reachable control targets are resolved.
/// The contract fixes mapped image bytes and permissions, excludes concurrent
/// mutation and calls, and does not certify binary patching or unwind behavior.
/// Ordinary ABI returns require every external-origin STORE target range to be
/// disjoint from the entry return-address slot (a caller/environment
/// precondition, also for computed external addresses). Frame-derived writes
/// are checked, and stack pivots, callee-pop returns, and RET-based dispatch
/// remain unsupported. On failure returns null and sets the session error.
/// Report, when nonnull, receives an owned JSON-v1 diagnostic/evidence document
/// even on failure. Free both returned strings with neverd_free_string().
/// Session caches are unaffected; recovery executes in its own transaction.
NEVERD_API const char *
neverd_devirtualize_source_v1(neverd_session_t Session, neverd_va_t Entry,
                              const neverd_devirtualize_options_v1 *Options,
                              const char **Report);

#ifdef __cplusplus
}
#endif
#endif
