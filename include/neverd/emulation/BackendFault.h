//===- BackendFault.h - Architecture-independent CPU faults --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_BACKENDFAULT_H
#define NEVERD_EMULATION_BACKENDFAULT_H
#include <cstdint>
#include <optional>

namespace neverd::emulation {
enum class BackendFaultKind {
#define NEVERD_BACKEND_FAULT_KIND(Name, Spelling) Name,
#include "neverd/emulation/BackendFaults.def"
#undef NEVERD_BACKEND_FAULT_KIND
};
enum class BackendAccessKind {
#define NEVERD_BACKEND_ACCESS_KIND(Name, Spelling) Name,
#include "neverd/emulation/BackendFaults.def"
#undef NEVERD_BACKEND_ACCESS_KIND
};
const char *backendFaultKindName(BackendFaultKind Kind);
const char *backendAccessKindName(BackendAccessKind Kind);
struct BackendFault {
  BackendFaultKind Kind;
  uint64_t PC = 0;
  /// Memory-event address and access size, not a decoded operand extent.
  /// Unicorn may split a memory access at a page boundary.
  std::optional<uint64_t> Address;
  std::optional<uint64_t> Size;
  std::optional<BackendAccessKind> Access;
  std::optional<uint32_t> Interrupt;
};
} // namespace neverd::emulation
#endif
