//===- InterpreterMachineStateProfile.h - Recovery environment -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_INTERPRETERMACHINESTATEPROFILE_H
#define NEVERD_ANALYSIS_INTERPRETERMACHINESTATEPROFILE_H

#include <cstdint>

namespace neverd::analysis {

enum class InterpreterMachineStateProfile : uint8_t {
  /// 64-bit user mode, CPL=3, IOPL=0, shadow stacks disabled, no asynchronous
  /// events or debug-exception delivery, and successful nonfaulting ordinary
  /// execution. TF, RF, VM, AC, VIF and VIP are zero at entry; every executed
  /// POPFQ image must keep TF and AC zero. Reserved bits have their
  /// architectural
  /// values. These conditions are explicit preconditions, not inferred from a
  /// successful run. TF/AC rejection restricts this profile; it is not a model
  /// of a hardware exception caused immediately by POPFQ.
  UserX64NoFaultV1,
};

/// A provider-owned projection whose meaning requires an explicit environment.
/// It never promotes the original architectural undefined-output sidecar.
enum class InterpreterProfileProjection : uint8_t {
  None,
  CetDisabledReadShadowStackV1,
};

} // namespace neverd::analysis

#endif
