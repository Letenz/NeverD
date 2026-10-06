//===- DriverScheduling.h - Explicit driver preemption policy ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_DRIVERSCHEDULING_H
#define NEVERD_EMULATION_DRIVERSCHEDULING_H

#include <cstdint>

namespace neverd::emulation {
namespace driver_scheduling {
#define NEVERD_DRIVER_SCHEDULING_VALUE(Name, Value)                            \
  inline constexpr uint64_t Name = Value;
#define NEVERD_DRIVER_SCHEDULING_FIELD(Name, Text)                             \
  inline constexpr char Name[] = Text;
#define NEVERD_DRIVER_SCHEDULING_DIAGNOSTIC(Name, Text)                        \
  inline constexpr char Name[] = Text;
#include "neverd/emulation/DriverScheduling.def"
} // namespace driver_scheduling

/// A reproducible CPU0 schedule. Instruction time is an explicit virtual
/// observation policy, not a hardware performance estimate. Instructions use
/// the workload's existing admitted machine-instruction attempt account.
struct DriverScheduling {
  uint64_t QuantumInstructions = driver_scheduling::DefaultQuantumInstructions;
  uint64_t InstructionTime100ns =
      driver_scheduling::DefaultInstructionTime100ns;
};
} // namespace neverd::emulation
#endif
