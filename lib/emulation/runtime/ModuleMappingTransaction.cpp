//===- ModuleMappingTransaction.cpp - Atomic RAM and instance publication
//---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Atomic RAM and instance publication.
///

#include "neverd/emulation/ExecutionLimits.h"
#include "neverd/emulation/Limits.h"
#include "neverd/emulation/ModuleBinding.h"

#include "llvm/ADT/ScopeExit.h"

#include <algorithm>
#include <set>

namespace neverd::emulation {
namespace {
llvm::Error failure(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
bool contains(uint64_t Base, uint64_t Size, uint64_t A, uint64_t N) {
  return N && A >= Base && A - Base < Size && N <= Size - (A - Base);
}
bool permitted(const ModuleMappingPlan &P, uint64_t A, unsigned Permission) {
  return std::any_of(P.Image.Regions.begin(), P.Image.Regions.end(),
                     [&](const auto &R) {
                       return contains(R.Address, R.Bytes.size(), A, 1) &&
                              (R.Permissions & Permission) == Permission;
                     });
}
llvm::Error validateExports(const ModuleBinding &B, uint64_t Base,
                            uint64_t Size, llvm::ArrayRef<ModuleExport> Exports,
                            llvm::ArrayRef<ModuleMappingCandidate> Candidates,
                            bool AllowForwarding, bool ObserveOnly = false) {
  if (Exports.size() > ::neverd::limits::emulation::
                           kModuleMappingTransactionValidateExportsSizeLimit)
    return failure("module export limit exhausted");
  std::map<std::string, const ModuleExport *> Names;
  std::map<uint32_t, const ModuleExport *> Ordinals;
  for (const auto &E : Exports) {
    if ((E.Name.empty() && !E.Ordinal) ||
        E.Name.size() >
            ::neverd::limits::emulation::
                kModuleMappingTransactionValidateExportsENameSizeLimit ||
        E.Name.find('\0') != std::string::npos ||
        (E.Kind != modules::SymbolKind::Function &&
         E.Kind != modules::SymbolKind::Data) ||
        (!E.Forwarded && !contains(Base, Size, E.Address, 1)))
      return failure("module export is invalid or outside its bound image");
    if (E.Forwarded) {
      if (!AllowForwarding)
        return failure("forwarded export requires environment finalization");
      const auto Permission =
          E.Kind == modules::SymbolKind::Function ? Execute : Read;
      const bool Planned =
          std::any_of(Candidates.begin(), Candidates.end(), [&](const auto &C) {
            return permitted(C.Plan, E.Address, Permission);
          });
      if (!Planned && !ObserveOnly) {
        auto Access = B.Space->canAccess(E.Address, 1, Permission);
        if (!Access)
          return Access.takeError();
        if (!*Access)
          return failure("forwarded export target has no permitted mapping: " +
                         B.GuestName + "!" + E.Name + " at " +
                         llvm::Twine(E.Address));
        auto RAM = B.Space->pinBacking(E.Address, 1);
        if (!RAM)
          return RAM.takeError();
      }
    }
    if (!E.Name.empty() && !Names.emplace(E.Name, &E).second)
      return failure("duplicate module export name");
    if (E.Ordinal) {
      const auto [I, Added] = Ordinals.emplace(*E.Ordinal, &E);
      if (!Added &&
          (I->second->Address != E.Address || I->second->Kind != E.Kind))
        return failure("ambiguous module export ordinal");
    }
  }
  return llvm::Error::success();
}
llvm::Error validate(const ModuleBinding &B, const ModuleMappingPlan &P,
                     uint64_t Budget, bool Final,
                     llvm::ArrayRef<ModuleMappingCandidate> Candidates = {},
                     bool AllowForwarding = false) {
  if (!P.Size || P.Size - 1 > UINT64_MAX - P.Image.Base || P.Size > Budget ||
      P.Image.Regions.empty() || (B.Base && P.Image.Base != *B.Base) ||
      (B.Size && P.Size != *B.Size))
    return failure(
        "module mapping does not match its declared extent or budget");
  uint64_t Bytes = 0;
  for (const auto &R : P.Image.Regions) {
    if (!contains(P.Image.Base, P.Size, R.Address, R.Bytes.size()) ||
        R.Bytes.size() > Budget - Bytes)
      return failure(
          "module mapping regions exceed the bound extent or budget");
    Bytes += R.Bytes.size();
  }
  if (Bytes != P.Image.MappedBytes ||
      (P.Image.Entry && !permitted(P, P.Image.Entry, Execute)))
    return failure("invalid module mapped-byte count or entry permission");
  if (!Final)
    return llvm::Error::success();
  if (P.Preparation)
    return failure(
        "module mapping requires completed environment finalization");
  return validateExports(B, P.Image.Base, P.Size, P.Exports, Candidates,
                         AllowForwarding);
}

} // namespace
llvm::Error ModuleBindings::adopt(llvm::ArrayRef<ModuleInstance> Adopted,
                                  const ModuleAdoptionValidator &Validator) {
  if (Busy || Pending || Adopted.empty() || !Validator ||
      Revision == UINT64_MAX)
    return failure("module adoption requires an available batch and validator");
  Busy = true;
  auto Release = llvm::scope_exit([&] { Busy = false; });
  const auto Space = Adopted.front().Binding.Space;
  if (!Space)
    return failure("module adoption requires an address space");
  if (auto E = Space->validateBacking(0, 0))
    return E;
  auto Next = Instances;
  for (const auto &I : Adopted) {
    const auto &B = I.Binding;
    if (B.Space != Space || !I.EnvironmentOwned || !I.Source ||
        (I.Load != modules::LoadPolicy::Eager &&
         I.Load != modules::LoadPolicy::Lazy) ||
        !I.Size || I.Size - 1 > UINT64_MAX - I.Base || I.Base % 4096 ||
        (I.Entry && !contains(I.Base, I.Size, I.Entry, 1)) || !B.Base ||
        *B.Base != I.Base || !B.Size || *B.Size != I.Size || I.Role.empty() ||
        I.Role.size() > execution_limit::NameBytes ||
        I.Role.find('\0') != std::string::npos)
      return failure("invalid preexisting module ownership or extent");
    if (auto E = validateDeclaration(B, Next, false))
      return E;
    // The environment validator owns these preexisting observations. Opaque
    // export addresses convey no RAM permissions; mapping new guest modules
    // still requires backed forwarder targets through the path above.
    if (auto E = validateExports(B, I.Base, I.Size, I.Exports, {}, true, true))
      return E;
    Next.emplace(B.ID, I);
  }
  if (auto E = Validator(Adopted))
    return E;
  std::unique_ptr<ModulePublication> Metadata;
  if (Publication) {
    std::vector<ModuleInstance> Preview;
    for (const auto &[ID, I] : Next)
      Preview.push_back(I);
    auto Prepared = Publication(Preview);
    if (!Prepared)
      return Prepared.takeError();
    if (!*Prepared)
      return failure("module publication preparer returned no transaction");
    Metadata = std::move(*Prepared);
  }
  const auto Patches =
      Metadata ? Metadata->patches() : llvm::ArrayRef<BackingPatch>();
  auto Transaction = Patches.empty() ? Space->prepareMetadataPublication()
                                     : Space->prepareMappings({}, Patches);
  if (!Transaction)
    return Transaction.takeError();
  if (auto E = (*Transaction)->commit())
    return E;
  Instances.swap(Next);
  ++Revision;
  if (Metadata)
    Metadata->commit();
  return llvm::Error::success();
}
llvm::Error ModuleBindings::materializeImpl(
    llvm::ArrayRef<ModuleBinding> Bindings, const ModuleMappingBuilder &Builder,
    const ModuleRequest *Request,
    std::shared_ptr<AddressSpace> PublicationSpace) {
  if (Busy || (Bindings.empty() && !PublicationSpace) || !Builder ||
      Revision == UINT64_MAX)
    return failure("module materialization requires an available nonempty "
                   "batch and builder");
  Busy = true;
  auto Release = llvm::scope_exit([&] { Busy = false; });
  std::vector<ModuleBinding> Working(Bindings.begin(), Bindings.end());
  const auto Space =
      PublicationSpace ? PublicationSpace : Bindings.front().Space;
  std::set<std::string> InitialIDs;
  for (const auto &B : Bindings)
    InitialIDs.insert(B.ID);
  auto Attempt = [&](llvm::ArrayRef<ModuleBinding> Bindings) -> llvm::Error {
    auto Next = Instances;
    if (!Space || (Request && Request->Space != Space))
      return failure("resource supply targets another address space");
    if (auto E = Space->validateBacking(0, 0))
      return E;
    std::set<std::string> BatchIDs;
    for (const auto &B : Bindings) {
      if (B.Space != Space || !BatchIDs.insert(B.ID).second)
        return failure("module transaction requires unique instances in one "
                       "address space");
      if (const auto I = Next.find(B.ID); I != Next.end()) {
        if (I->second.mapped() || I->second.Binding != B)
          return failure(
              "module supply replaces an existing identity or binding");
      } else {
        if (auto E = validateDeclaration(B, Next))
          return E;
        ModuleInstance New;
        New.Binding = B;
        for (const auto &Input : Sources->inspect())
          if (Input.Input.ID == B.ModuleID)
            New.Load = Input.Load;
        Next.emplace(B.ID, std::move(New));
      }
    }
    if (Request && Request->InstanceID && !BatchIDs.count(*Request->InstanceID))
      return failure("module supply does not satisfy the requested instance");
    if (Request && Request->Kind == ModuleRequestKind::MissingDependency &&
        std::none_of(Bindings.begin(), Bindings.end(), [&](const auto &B) {
          return B.GuestName == Request->Dependency ||
                 B.ModuleID == Request->Dependency ||
                 (DependencyMatcher &&
                  DependencyMatcher(Request->Dependency, B));
        }))
      return failure(
          "module supply does not satisfy the requested dependency identity");
    std::vector<ModuleMappingPlan> Plans;
    Plans.reserve(Bindings.size());
    uint64_t PlannedBytes = 0;
    for (const auto &B : Bindings) {
      auto Source = Sources->prepare(B.ModuleID);
      if (!Source && !InitialIDs.contains(B.ID))
        return llvm::make_error<ModuleDependencyError>(
            B.GuestName, B.ID, llvm::toString(Source.takeError()));
      if (!Source)
        return Source.takeError();
      auto Plan = Builder(B, **Source);
      if (!Plan)
        return Plan.takeError();
      if (auto E = validate(B, *Plan, Space->physicalMemory()->limit(), false))
        return E;
      if (Plan->Image.MappedBytes >
          Space->physicalMemory()->limit() - PlannedBytes)
        return failure("module batch mapped-byte budget exhausted");
      PlannedBytes += Plan->Image.MappedBytes;
      auto &I = Next.at(B.ID);
      I.Source = std::move(*Source);
      I.Base = Plan->Image.Base;
      I.Size = Plan->Size;
      I.Entry = Plan->Image.Entry;
      Plans.push_back(std::move(*Plan));
    }
    std::vector<ModuleMappingCandidate> Candidates;
    for (size_t N = 0; N < Bindings.size(); ++N)
      Candidates.push_back(
          {Bindings[N], *Next.at(Bindings[N].ID).Source, Plans[N]});
    if (Finalizer && !Candidates.empty()) {
      std::vector<ModuleInstance> Existing;
      for (const auto &[ID, I] : Instances)
        Existing.push_back(I);
      if (auto E = Finalizer(Existing, Candidates))
        return E;
    }
    PlannedBytes = 0;
    for (const auto &C : Candidates) {
      if (auto E = validate(C.Binding, C.Plan, Space->physicalMemory()->limit(),
                            true, Candidates, bool(Finalizer)))
        return E;
      if (C.Plan.Image.MappedBytes >
          Space->physicalMemory()->limit() - PlannedBytes)
        return failure("linked module batch mapped-byte budget exhausted");
      PlannedBytes += C.Plan.Image.MappedBytes;
      auto &I = Next.at(C.Binding.ID);
      I.Base = C.Plan.Image.Base;
      I.Size = C.Plan.Size;
      I.Entry = C.Plan.Image.Entry;
      I.Exports = std::move(C.Plan.Exports);
      I.RuntimeData = std::move(C.Plan.RuntimeData);
    }
    // Validate resolved extents as well as declarations. Reserving only actual
    // mapped pages must not permit another instance inside a module's holes.
    for (auto A = Next.begin(); A != Next.end(); ++A) {
      const auto &I = A->second;
      const auto Base = I.mapped() ? std::optional(I.Base) : I.Binding.Base;
      const auto Size = I.mapped() ? std::optional(I.Size) : I.Binding.Size;
      if (I.Binding.Space != Space || !Base || !Size)
        continue;
      for (auto B = std::next(A); B != Next.end(); ++B) {
        const auto &J = B->second;
        const auto Other = J.mapped() ? std::optional(J.Base) : J.Binding.Base;
        const auto Count = J.mapped() ? std::optional(J.Size) : J.Binding.Size;
        if (J.Binding.Space == Space && Other && Count &&
            (*Base <= *Other ? *Other - *Base < *Size
                             : *Base - *Other < *Count))
          return failure("resolved module extents overlap");
      }
    }
    std::vector<InitializedMapping> Mappings;
    for (const auto &Plan : Plans)
      for (const auto &R : Plan.Image.Regions)
        Mappings.push_back({R.Address, R.Permissions, R.Bytes});
    if (Request && Request->Size) {
      const unsigned Permission = Request->RequiredPermissions;
      uint64_t A = Request->Address, Remaining = Request->Size;
      bool NewCoverage = false;
      while (Remaining) {
        const auto R =
            std::find_if(Mappings.begin(), Mappings.end(), [&](const auto &R) {
              return contains(R.Address, R.Bytes.size(), A, 1);
            });
        uint64_t N;
        if (R != Mappings.end()) {
          if ((R->Permissions & Permission) != Permission)
            return failure(
                "supplied mapping lacks requested access permission");
          N = std::min(Remaining, uint64_t(R->Bytes.size()) - (A - R->Address));
          NewCoverage = true;
        } else {
          N = std::min(Remaining, 4096 - A % 4096);
          auto Access = Space->canAccess(A, N, Permission);
          if (!Access)
            return Access.takeError();
          if (!*Access)
            return failure(
                "supplied mapping leaves a hole in the requested access");
        }
        Remaining -= N;
        if (Remaining)
          A += N;
      }
      if (!NewCoverage)
        return failure("supplied mapping does not cover the requested address");
    }
    std::unique_ptr<ModulePublication> Metadata;
    if (Publication) {
      std::vector<ModuleInstance> Preview;
      for (const auto &[ID, I] : Next)
        Preview.push_back(I);
      auto Prepared = Publication(Preview);
      if (!Prepared)
        return Prepared.takeError();
      if (!*Prepared)
        return failure("module publication preparer returned no transaction");
      Metadata = std::move(*Prepared);
    }
    const auto Patches =
        Metadata ? Metadata->patches() : llvm::ArrayRef<BackingPatch>();
    auto Transaction = Mappings.empty() && Patches.empty()
                           ? Space->prepareMetadataPublication()
                           : Space->prepareMappings(Mappings, Patches);
    if (!Transaction)
      return Transaction.takeError();
    // The transaction retains the stopped lease through both metadata swaps.
    // No fallible work or backend projection follows the memory commit.
    if (auto E = (*Transaction)->commit())
      return E;
    Instances.swap(Next);
    ++Revision;
    if (Metadata)
      Metadata->commit();
    return llvm::Error::success();
  };
  while (true) {
    auto E = Attempt(Working);
    if (!E)
      return llvm::Error::success();
    std::string Dependency, Detail;
    std::optional<std::string> ID;
    bool Missing = false;
    auto Other =
        llvm::handleErrors(std::move(E), [&](const ModuleDependencyError &D) {
          Missing = true;
          Dependency = D.Dependency;
          ID = D.InstanceID;
          Detail = D.Detail;
        });
    if (!Missing)
      return Other;
    if (Other)
      return llvm::joinErrors(
          std::move(Other),
          llvm::make_error<ModuleDependencyError>(Dependency, ID, Detail));
    const auto I = ID ? Instances.find(*ID) : Instances.end();
    if (I == Instances.end() || I->second.mapped() || Working.size() >= Limit ||
        I->second.Binding.Space != Space ||
        std::any_of(Working.begin(), Working.end(),
                    [&](const auto &B) { return B.ID == *ID; }))
      return llvm::make_error<ModuleDependencyError>(Dependency, ID, Detail);
    Working.push_back(I->second.Binding);
  }
}
llvm::Error
ModuleBindings::refreshPublication(const std::shared_ptr<AddressSpace> &Space,
                                   const ModuleMappingBuilder &Builder) {
  if (!Space || !Publication || Pending)
    return failure(
        "module metadata refresh requires an available publication adapter");
  return materializeImpl({}, Builder, nullptr, Space);
}
} // namespace neverd::emulation
