//===- ExecutionRunJSON.h - Shared public workload protocol -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared public workload protocol.
///

#ifndef NEVERD_EMULATION_EXECUTIONRUNJSON_H
#define NEVERD_EMULATION_EXECUTIONRUNJSON_H
#include "neverd/emulation/ExecutionLimits.h"
#include "neverd/emulation/ExecutionRun.h"

namespace neverd::emulation {
/// Wire declarations contain scene-owned names, never host addresses. Empty
/// AddressSpace requests the scene's explicit default. Parsing performs no IO,
/// CPU access, source preparation, address-space selection or guest mutation.
struct ExecutionBindingInput {
  std::string ID, ModuleID, GuestName, Scope, AddressSpace;
  std::optional<uint64_t> Base, Size;
};
struct ExecutionSupplyInput {
  ModuleRequestToken Token;
  std::vector<ExecutionBindingInput> Bindings;
};
struct ExecutionCreationInput {
  std::string EnvironmentKind, EnvironmentJSON;
  ExecutionRunOptions Options;
  std::vector<ExecutionBindingInput> Bindings;
};
/// Requires an explicit environment kind. The selected environment factory
/// owns validation of its configuration; this decoder selects no fallback.
llvm::Expected<ExecutionCreationInput>
    executionCreationFromJSON(llvm::StringRef);
using ExecutionSpaceResolver =
    std::function<llvm::Expected<std::shared_ptr<AddressSpace>>(
        llvm::StringRef)>;
using ExecutionSpaceIdentifier = std::function<llvm::Expected<std::string>(
    const std::shared_ptr<AddressSpace> &)>;

/// Control options only; Bindings are decoded separately, then resolved by
/// the environment after its address spaces exist. ExecutionRun still owns
/// state, lifecycle, quota and environment capability admission.
llvm::Expected<ExecutionRunOptions>
    executionRunOptionsFromJSON(llvm::StringRef);
llvm::Expected<std::vector<ExecutionBindingInput>>
    executionBindingsFromJSON(llvm::StringRef);
llvm::Expected<ExecutionSupplyInput> executionSupplyFromJSON(llvm::StringRef);
llvm::Expected<GuestInvocationRequest>
guestInvocationFromJSON(llvm::StringRef,
                        uint64_t ByteLimit = execution_limit::TransferBytes);
/// Resolve all declarations into a local batch. The callback must be a pure
/// lookup in the stopped scene; unknown names/null results fail, with no guest
/// publication. Empty batches do not require a resolver.
llvm::Expected<std::vector<ModuleBinding>>
resolveExecutionBindings(llvm::ArrayRef<ExecutionBindingInput>,
                         const ExecutionSpaceResolver &);

/// Version 1 reports encode every uint64 as a hexadecimal string, including
/// request ownership/version/sequence, addresses and accumulated quotas.
/// Address-space names must come from the scene. No pointer string fallback.
/// Returned text is owned. Optional facts are omitted rather than invented.
llvm::Expected<std::string>
executionRunEventJSON(const ExecutionRunEvent &,
                      const ExecutionSpaceIdentifier & = {});
llvm::Expected<std::string>
executionRunSnapshotJSON(const ExecutionRunSnapshot &,
                         const ExecutionSpaceIdentifier & = {});
llvm::Expected<std::string>
executionInstancesJSON(llvm::ArrayRef<ModuleInstance>,
                       const ExecutionSpaceIdentifier &);
} // namespace neverd::emulation
#endif
