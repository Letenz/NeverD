//===- ResourceEvent.h - Validated resource stops --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Validated resource stops.
///

#ifndef NEVERD_EMULATION_RESOURCEEVENT_H
#define NEVERD_EMULATION_RESOURCEEVENT_H
#include "neverd/emulation/BackendFault.h"

namespace neverd::emulation {
enum class ResourceRestartKind {
#define NEVERD_RESOURCE_RESTART(Name, Text) Name,
#include "neverd/emulation/ResourceEvent.def"
#undef NEVERD_RESOURCE_RESTART
};
const char *resourceRestartKindName(ResourceRestartKind Kind);
/// A backend-validated resource boundary, independent of OS exception transfer.
/// ResumePC is the actual machine boundary, never a caller-chosen rewind.
/// ArchitecturalRetry may retain instruction-prefix stores; RepeatProgress
/// retains completed iterations. Registers and RAM are not rolled back.
/// Hooks see a provisional candidate; only a typed ResourcePause exit publishes
/// the final validation result and authorizes mapping repair/acknowledgement.
struct ResourcePause {
  BackendFault Fault;
  uint64_t ResumePC = 0, InstructionPC = 0;
  ResourceRestartKind Restart = ResourceRestartKind::ArchitecturalRetry;
  bool MemoryEffectsMayExist = false;
  /// Exact guest permission mask, including privilege and combined operands.
  unsigned RequiredPermissions = 0;
};
} // namespace neverd::emulation
#endif
