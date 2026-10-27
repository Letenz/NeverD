//===- ModuleHandle.h - Shared source ownership behind C handles -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared source ownership behind C handles.
///

#ifndef NEVERD_SDK_MODULEHANDLE_H
#define NEVERD_SDK_MODULEHANDLE_H
#include "ResourceHandles.h"

#include "neverd/modules/ModuleSet.h"

#include <atomic>

namespace neverd::sdk::resources {
/// Execution consumers retain this same state after the public source handle
/// is destroyed, and coordinate all mutable ModuleSet/query access with Busy.
struct ModuleHandleState {
  std::shared_ptr<modules::ModuleSet> Sources;
  std::filesystem::path BaseDirectory;
  std::atomic<bool> Busy{false};
};
llvm::Expected<std::shared_ptr<ModuleHandleState>> moduleState(uint64_t);
class ModuleAccess final {
public:
  ModuleAccess(ModuleAccess &&) noexcept = default;
  ModuleAccess(const ModuleAccess &) = delete;
  ~ModuleAccess() {
    if (State)
      State->Busy.store(false);
  }
  ModuleHandleState &state() const { return *State; }
  std::shared_ptr<ModuleHandleState> sharedState() const { return State; }

private:
  friend llvm::Expected<ModuleAccess> accessModule(uint64_t);
  friend llvm::Expected<ModuleAccess>
      accessModule(std::shared_ptr<ModuleHandleState>);
  explicit ModuleAccess(std::shared_ptr<ModuleHandleState> State)
      : State(std::move(State)) {}
  std::shared_ptr<ModuleHandleState> State;
};
llvm::Expected<ModuleAccess> accessModule(uint64_t);
llvm::Expected<ModuleAccess> accessModule(std::shared_ptr<ModuleHandleState>);
/// Shared primitive implementations. Caller holds the source operation guard.
/// They preserve the canonical origin, input parser, identity and IO policy.
llvm::Error addModuleJSON(ModuleHandleState &, const char *);
llvm::Error reviseModuleJSON(ModuleHandleState &, const char *);
llvm::Error attachModuleDebugJSON(ModuleHandleState &, const char *ID,
                                  const char *);
} // namespace neverd::sdk::resources
#endif
