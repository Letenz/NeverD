//===- ExecutionRunFactory.h - Scene creation result ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Scene creation result.
///

#ifndef NEVERD_EMULATION_EXECUTIONRUNFACTORY_H
#define NEVERD_EMULATION_EXECUTIONRUNFACTORY_H
#include "neverd/emulation/ExecutionRunJSON.h"

namespace neverd::emulation {
/// A frontend retains this run and the scene-owned naming callbacks. The run
/// remains the sole CPU/environment/budget owner. No OS policy lives here.
struct ExecutionRunFactoryResult {
  std::unique_ptr<ExecutionRun> Run;
  ExecutionSpaceResolver ResolveSpace;
  ExecutionSpaceIdentifier IdentifySpace;
  uint64_t MemoryTransferLimit = 0;
  /// Optional stopped environment observations, encoded by that environment.
  /// It must not run guest code or mutate the workload lifecycle.
  std::function<llvm::Expected<std::string>()> InspectEnvironment;
};
/// Explicit raw CPU buffers, not an OS/application loader. Binary resources
/// may only map fixed link-time segments without imports or relocations.
llvm::Expected<ExecutionRunFactoryResult>
    createBufferExecution(ExecutionCreationInput,
                          std::shared_ptr<modules::ModuleSet>);
} // namespace neverd::emulation
#endif
