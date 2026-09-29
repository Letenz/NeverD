//===- ExecutionReport.h - Validated CPU query JSON ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_EXECUTIONREPORT_H
#define NEVERD_EMULATION_EXECUTIONREPORT_H

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ExecutionReportFields.h"

namespace neverd::emulation {
/// Unknown fields, null values, invalid names and unsupported requirements are
/// rejected. Missing fields use ExecutionConfiguration's documented defaults.
llvm::Expected<ExecutionConfiguration>
executionConfigurationFromJSON(llvm::StringRef Text);

/// Reports the same resolved configuration used by createExecutionBackend.
/// Build support is queried without allocating a CPU. Host is null unless an
/// explicit temporary initialization probe is requested. Neither report mode
/// establishes workload compatibility or sustained execution coverage.
llvm::Expected<std::string>
executionCapabilitiesJSON(const ExecutionConfiguration &Configuration,
                          bool ProbeHost = false);
} // namespace neverd::emulation
#endif
