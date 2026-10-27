//===- ModuleBinding.h - Per-workload module instances -----------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Per-workload module instances.
///

#ifndef NEVERD_EMULATION_MODULEBINDING_H
#define NEVERD_EMULATION_MODULEBINDING_H

#include "neverd/emulation/ImageMapping.h"
#include "neverd/emulation/Limits.h"
#include "neverd/emulation/ModuleRequest.h"
#include "neverd/modules/ModuleSet.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neverd::emulation {
struct ModuleBinding {
  std::string ID, ModuleID, GuestName, Scope;
  std::shared_ptr<AddressSpace> Space;
  std::optional<uint64_t> Base, Size;
  bool operator==(const ModuleBinding &) const = default;
};
struct ModuleExport {
  std::string Name;
  std::optional<uint32_t> Ordinal;
  uint64_t Address = 0;
  modules::SymbolKind Kind = modules::SymbolKind::Function;
  /// An environment-resolved forwarder may target another mapped owner.
  /// This remains address metadata and conveys no execution/model authority.
  bool Forwarded = false;
};
// Export addresses are metadata. Neither symbol kind nor successful lookup
// grants memory permission or execution support for an opaque runtime object.
/// The environment owns relocation, import/forwarder resolution, privilege,
/// header visibility and opaque runtime data. Return the prepared byte layout
/// and final permissions; optional Preparation requires a batch Finalizer.
/// No CPU, OS table or live memory may change in the builder.
class ModuleMappingPreparation {
public:
  virtual ~ModuleMappingPreparation() = default;
  virtual const void *typeID() const = 0;
};
using ModuleDependencyMatcher =
    std::function<bool(llvm::StringRef, const ModuleBinding &)>;
/// Immutable adapter facts prepared from a mapping's final bytes. The common
/// layer retains them but grants no OS or execution authority from their type.
class ModuleRuntimeData {
public:
  virtual ~ModuleRuntimeData() = default;
  virtual const void *typeID() const = 0;
};
struct ModuleMappingPlan {
  ImageMappingPlan Image;
  uint64_t Size = 0;
  std::vector<ModuleExport> Exports;
  /// Optional immutable adapter-owned linker input, consumed by Finalizer.
  /// It never becomes live instance state or a source-file observation.
  std::shared_ptr<const ModuleMappingPreparation> Preparation;
  std::shared_ptr<const ModuleRuntimeData> RuntimeData;
};
using ModuleMappingBuilder = std::function<llvm::Expected<ModuleMappingPlan>(
    const ModuleBinding &, const modules::ModuleImage &)>;
struct ModuleInstance {
  ModuleBinding Binding;
  modules::LoadPolicy Load = modules::LoadPolicy::Eager;
  /// Absent until committed. Source preparation alone is not a mapped instance.
  std::shared_ptr<const modules::ModuleImage> Source;
  uint64_t Base = 0, Size = 0, Entry = 0;
  std::vector<ModuleExport> Exports;
  /// Preexisting mapping owned and validated by the environment. Adoption
  /// publishes metadata only; it never grants permissions or initializes code.
  bool EnvironmentOwned = false;
  /// Opaque environment role; the common layer interprets no OS-specific role.
  std::string Role;
  std::shared_ptr<const ModuleRuntimeData> RuntimeData;
  bool mapped() const { return bool(Source); }
};
/// Pure proof of an environment's preexisting mapping/source ownership. No IO,
/// execution or live mutation. Sources may be pinned observations from a
/// catalogue that has already died; adoption never reopens their paths.
using ModuleAdoptionValidator =
    std::function<llvm::Error(llvm::ArrayRef<ModuleInstance>)>;
struct ModuleMappingCandidate {
  const ModuleBinding &Binding;
  const modules::ModuleImage &Source;
  ModuleMappingPlan &Plan;
};
/// Pure stopped linking over a whole batch and the previous instance table.
/// Only draft bytes/exports may change; consume Preparation on success. Failed
/// drafts are abandoned, including when a declared dependency expands a batch.
/// No file IO, CPU execution, live registry or OS mutation is permitted.
using ModuleMappingFinalizer = std::function<llvm::Error(
    llvm::ArrayRef<ModuleInstance>, llvm::ArrayRef<ModuleMappingCandidate>)>;
struct ModuleRange {
  uint64_t Base, Size;
  bool Mapped;
};
/// Environment metadata staged without visible effects. Destruction abandons
/// it. Commit must only swap already prepared state: no allocation, failure,
/// CPU execution, callbacks, file IO or memory mutation. The RAM transaction
/// retains its stopped lease until this publication finishes.
class ModulePublication {
public:
  virtual ~ModulePublication() = default;
  /// Prepared guest metadata writes join the RAM transaction. The returned
  /// byte spans must remain alive and immutable until this object is released.
  virtual llvm::ArrayRef<BackingPatch> patches() const { return {}; }
  virtual void commit() noexcept = 0;
};
using ModulePublicationPreparer =
    std::function<llvm::Expected<std::unique_ptr<ModulePublication>>(
        llvm::ArrayRef<ModuleInstance>)>;

/// One execution's module table. Declarations, including lazy entries, are
/// visible without reading their files. Guest naming rules belong to the OS
/// adapter: names here are exact, case-sensitive identities within a scope.
/// All calls require external serialization, with CPUs stopped and outside
/// model callbacks. Source registration never mutates this table implicitly.
class ModuleBindings final {
public:
  static llvm::Expected<std::unique_ptr<ModuleBindings>> create(
      std::shared_ptr<modules::ModuleSet> Sources,
      uint64_t Limit = ::neverd::limits::emulation::kModuleBindingCreateLimit,
      ModulePublicationPreparer Publication = {},
      ModuleMappingFinalizer Finalizer = {},
      ModuleDependencyMatcher DependencyMatcher = {});
  ModuleBindings(const ModuleBindings &) = delete;
  ModuleBindings &operator=(const ModuleBindings &) = delete;
  llvm::Error declare(ModuleBinding Binding);
  /// Publish preexisting instances in one space after adapter validation.
  /// Requires stopped CPUs, no pending request and fresh instance identities.
  /// The validator proves export ownership too. Opaque external addresses are
  /// metadata only; adoption never grants backing or access permissions.
  /// Existing RAM and the environment publication join one stopped lease.
  llvm::Error adopt(llvm::ArrayRef<ModuleInstance>,
                    const ModuleAdoptionValidator &);
  /// Map a batch in one address space. Existing declarations must match
  /// exactly; previously undeclared bindings publish only on success. Source
  /// caching may advance on failure; guest memory and module state do not.
  llvm::Error materialize(llvm::ArrayRef<ModuleBinding> Bindings,
                          const ModuleMappingBuilder &Builder);
  /// Refresh adapter metadata over this table. Declared dependencies may
  /// expand the transaction through Builder, exactly as materialization does.
  /// No pending request is allowed; failure publishes nothing.
  llvm::Error refreshPublication(const std::shared_ptr<AddressSpace> &,
                                 const ModuleMappingBuilder &);
  /// Uses the same transaction path, excluding lazy declarations. Separate
  /// address spaces require separate batches; no cross-space atomicity claim.
  llvm::Error prepareEager(const std::shared_ptr<AddressSpace> &Space,
                           const ModuleMappingBuilder &Builder);
  std::vector<ModuleInstance> inspect() const;
  /// Small metadata snapshots for stopped consumers; never copy exports or IO.
  std::vector<ModuleBinding>
  unmappedBindings(const std::shared_ptr<AddressSpace> &Space,
                   std::optional<modules::LoadPolicy> Policy = {}) const;
  std::vector<ModuleRange>
  ranges(const std::shared_ptr<AddressSpace> &Space) const;
  std::optional<ModuleInstance> find(llvm::StringRef ID) const;
  llvm::Expected<std::optional<uint64_t>>
  lookupSymbol(llvm::StringRef ID, llvm::StringRef Name) const;
  llvm::Expected<std::optional<ModuleExport>>
  lookupExport(llvm::StringRef ID, llvm::StringRef Name) const;
  llvm::Expected<std::optional<ModuleExport>>
  lookupExport(llvm::StringRef ID, uint32_t Ordinal) const;

  llvm::Expected<ModuleRequest>
  requestAddress(std::shared_ptr<AddressSpace> Space, uint64_t PC,
                 uint64_t Address, uint64_t Size, ModuleAccess Access,
                 unsigned RequiredPermissions = 0);
  llvm::Expected<ModuleRequest> requestModule(llvm::StringRef ID);
  llvm::Expected<ModuleRequest>
  requestDependency(std::shared_ptr<AddressSpace> Space,
                    llvm::StringRef Dependency, uint64_t PC = 0,
                    std::optional<std::string> InstanceID = {});
  std::optional<ModuleRequest> pendingRequest() const { return Pending; }
  /// Reject stale/cross-owner/consumed tokens before any file IO or builder.
  /// Failed supply preserves the pending request for correction and retry.
  llvm::Error supply(ModuleRequestToken Token,
                     llvm::ArrayRef<ModuleBinding> Bindings,
                     const ModuleMappingBuilder &Builder);
  llvm::Error cancel(ModuleRequestToken Token);
  uint64_t revision() const { return Revision; }
  modules::ModuleSet &sources() const { return *Sources; }

private:
  friend class ModuleResolverGuard;
  ModuleBindings(std::shared_ptr<modules::ModuleSet> Sources, uint64_t Limit,
                 uint64_t Owner, ModulePublicationPreparer Publication,
                 ModuleMappingFinalizer Finalizer,
                 ModuleDependencyMatcher DependencyMatcher)
      : Sources(std::move(Sources)), Publication(std::move(Publication)),
        Finalizer(std::move(Finalizer)),
        DependencyMatcher(std::move(DependencyMatcher)), Limit(Limit),
        Owner(Owner) {}
  llvm::Error
  validateDeclaration(const ModuleBinding &Binding,
                      const std::map<std::string, ModuleInstance> &Next,
                      bool RequireSource = true) const;
  llvm::Expected<ModuleRequest> issue(ModuleRequest Request);
  llvm::Error validateToken(ModuleRequestToken Token) const;
  llvm::Error
  materializeImpl(llvm::ArrayRef<ModuleBinding> Bindings,
                  const ModuleMappingBuilder &Builder,
                  const ModuleRequest *Request,
                  std::shared_ptr<AddressSpace> PublicationSpace = {});
  std::shared_ptr<modules::ModuleSet> Sources;
  ModulePublicationPreparer Publication;
  ModuleMappingFinalizer Finalizer;
  ModuleDependencyMatcher DependencyMatcher;
  uint64_t Limit, Owner, Revision = 0, Sequence = 0;
  std::map<std::string, ModuleInstance> Instances;
  std::optional<ModuleRequest> Pending;
  bool Busy = false;
};
} // namespace neverd::emulation
#endif
