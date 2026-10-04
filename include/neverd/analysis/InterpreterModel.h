//===- InterpreterModel.h - Shared analysis graph --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_ANALYSIS_INTERPRETERMODEL_H
#define NEVERD_ANALYSIS_INTERPRETERMODEL_H

#include "neverd/analysis/LowIRUndefinedIndependence.h"

#include <vector>

namespace neverd::analysis {

/// Generated LowIR graph and deterministic instruction records. Register
/// identities and observations are supplied by the selected model adapter;
/// these records are not native undefined-output or ABI evidence.
struct InterpreterMachineStateModel {
  LowFunc Function;
  /// Complete deterministic LowIR records for this generated model. These
  /// are not architecture-undefined-output evidence for the original binary.
  std::vector<LowIRUndefinedInstruction> Instructions;
};

} // namespace neverd::analysis

#endif
