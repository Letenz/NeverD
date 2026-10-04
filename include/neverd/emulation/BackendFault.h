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
enum class BackendFaultCause {
#define NEVERD_BACKEND_FAULT_CAUSE(Name, Spelling) Name,
#include "neverd/emulation/BackendFaults.def"
#undef NEVERD_BACKEND_FAULT_CAUSE
};
const char *backendFaultKindName(BackendFaultKind Kind);
const char *backendAccessKindName(BackendAccessKind Kind);
const char *backendFaultCauseName(BackendFaultCause Cause);
struct BackendFault {
  BackendFaultKind Kind;
  uint64_t PC = 0;
  /// Memory-event address and access size, not a decoded operand extent.
  /// Checked faults identify the first inaccessible page fragment. Unicorn
  /// may split a memory access at a page boundary.
  std::optional<uint64_t> Address;
  std::optional<uint64_t> Size;
  std::optional<BackendAccessKind> Access;
  std::optional<uint32_t> Interrupt;
  /// Processor-supplied exception code, interpreted only by the guest ISA/OS.
  /// Absence differs from a valid zero code; transport errors never invent one.
  std::optional<uint64_t> ErrorCode;
  /// Optional cause established by the architecture's instruction checks.
  /// A raw processor vector is insufficient to infer this classification.
  /// OperandAlignment currently identifies checked x64 aligned SSE operands;
  /// it does not imply that a data access or page lookup has occurred.
  std::optional<BackendFaultCause> Cause;
};
} // namespace neverd::emulation
#endif
