//===- ModuleBinding.cpp - Declarations and resource request ownership
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declarations and resource request ownership.
///

#include "neverd/emulation/ModuleBinding.h"

#include "neverd/emulation/ExecutionLimits.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <atomic>
#include <limits>

namespace neverd::emulation {
char ModuleDependencyError::ID;
void ModuleDependencyError::log(llvm::raw_ostream &OS) const {
  OS << "missing module dependency: " << Dependency;
  if (!Detail.empty())
    OS << "; " << Detail;
}
std::error_code ModuleDependencyError::convertToErrorCode() const {
  return llvm::inconvertibleErrorCode();
}
namespace {
std::atomic<uint64_t> NextOwner{1};
llvm::Error failure(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
bool text(llvm::StringRef S, size_t Limit, bool Empty = false) {
  return (Empty || !S.empty()) && S.size() <= Limit &&
         S.find('\0') == llvm::StringRef::npos;
}
bool extent(uint64_t Base, uint64_t Size) {
  return Size && Size - 1 <= UINT64_MAX - Base;
}
bool contains(uint64_t Base, uint64_t Size, uint64_t Address, uint64_t Count) {
  return extent(Address, Count) && Address >= Base && Address - Base < Size &&
         Count <= Size - (Address - Base);
}
bool overlaps(uint64_t A, uint64_t N, uint64_t B, uint64_t M) {
  return A <= B ? B - A < N : A - B < M;
}
} // namespace
llvm::Expected<std::unique_ptr<ModuleBindings>>
ModuleBindings::create(std::shared_ptr<modules::ModuleSet> Sources,
                       uint64_t Limit, ModulePublicationPreparer Publication,
                       ModuleMappingFinalizer Finalizer,
                       ModuleDependencyMatcher DependencyMatcher) {
  if (!Sources || !Limit || Limit > execution_limit::Instances)
    return failure("invalid module binding resources or limit");
  // Never reuse an execution identity on sequence exhaustion.
  uint64_t Owner = NextOwner.load();
  do {
    if (Owner == UINT64_MAX)
      return failure("module binding owner identities exhausted");
  } while (!NextOwner.compare_exchange_weak(Owner, Owner + 1));
  return std::unique_ptr<ModuleBindings>(new ModuleBindings(
      std::move(Sources), Limit, Owner, std::move(Publication),
      std::move(Finalizer), std::move(DependencyMatcher)));
}
llvm::Error ModuleBindings::validateDeclaration(
    const ModuleBinding &B, const std::map<std::string, ModuleInstance> &Next,
    bool RequireSource) const {
  if (!text(B.ID, execution_limit::NameBytes) ||
      !text(B.ModuleID, execution_limit::NameBytes) ||
      !text(B.GuestName, execution_limit::GuestNameBytes) ||
      !text(B.Scope, execution_limit::NameBytes, true) || !B.Space ||
      (B.Size && (!B.Base || !extent(*B.Base, *B.Size))) ||
      (B.Base && *B.Base % 4096))
    return failure("invalid module binding declaration");
  const auto Inputs = Sources->inspect();
  if (RequireSource &&
      std::none_of(Inputs.begin(), Inputs.end(),
                   [&](const auto &I) { return I.Input.ID == B.ModuleID; }))
    return failure("unknown module resource: " + B.ModuleID);
  if (Next.size() >= Limit)
    return failure("module instance limit exhausted");
  for (const auto &[ID, I] : Next) {
    if (ID == B.ID)
      return failure("duplicate module instance: " + B.ID);
    if (I.Binding.Space != B.Space)
      continue;
    if (I.Binding.Scope == B.Scope && I.Binding.GuestName == B.GuestName)
      return failure("duplicate guest module name in address space and scope");
    const auto Base = I.mapped() ? std::optional(I.Base) : I.Binding.Base;
    const auto Size = I.mapped() ? std::optional(I.Size) : I.Binding.Size;
    if (B.Base && B.Size && Base && Size &&
        overlaps(*B.Base, *B.Size, *Base, *Size))
      return failure("overlapping declared module extents");
  }
  return llvm::Error::success();
}
llvm::Error ModuleBindings::declare(ModuleBinding Binding) {
  if (Busy || Pending || Revision == UINT64_MAX)
    return failure("module declaration is not allowed in the current state");
  if (auto E = validateDeclaration(Binding, Instances))
    return E;
  if (auto E = Binding.Space->validateBacking(0, 0))
    return E;
  const auto Inputs = Sources->inspect();
  const auto Policy =
      std::find_if(Inputs.begin(), Inputs.end(), [&](const auto &I) {
        return I.Input.ID == Binding.ModuleID;
      })->Load;
  const auto ID = Binding.ID;
  ModuleInstance I;
  I.Binding = std::move(Binding);
  I.Load = Policy;
  Instances.emplace(ID, std::move(I));
  ++Revision;
  return llvm::Error::success();
}
std::vector<ModuleInstance> ModuleBindings::inspect() const {
  std::vector<ModuleInstance> Result;
  for (const auto &[ID, Instance] : Instances)
    Result.push_back(Instance);
  return Result;
}
std::optional<ModuleInstance> ModuleBindings::find(llvm::StringRef ID) const {
  const auto I = Instances.find(ID.str());
  return I == Instances.end() ? std::nullopt : std::optional(I->second);
}
std::vector<ModuleBinding> ModuleBindings::unmappedBindings(
    const std::shared_ptr<AddressSpace> &Space,
    std::optional<modules::LoadPolicy> Policy) const {
  std::vector<ModuleBinding> Result;
  for (const auto &[ID, I] : Instances)
    if (I.Binding.Space == Space && !I.mapped() &&
        (!Policy || I.Load == *Policy))
      Result.push_back(I.Binding);
  return Result;
}
std::vector<ModuleRange>
ModuleBindings::ranges(const std::shared_ptr<AddressSpace> &Space) const {
  std::vector<ModuleRange> Result;
  for (const auto &[ID, I] : Instances) {
    if (I.Binding.Space != Space)
      continue;
    if (I.mapped())
      Result.push_back({I.Base, I.Size, true});
    else if (I.Binding.Base && I.Binding.Size)
      Result.push_back({*I.Binding.Base, *I.Binding.Size, false});
  }
  return Result;
}
llvm::Expected<std::optional<uint64_t>>
ModuleBindings::lookupSymbol(llvm::StringRef ID, llvm::StringRef Name) const {
  const auto I = find(ID);
  if (!I)
    return failure("unknown module instance: " + ID);
  if (!I->mapped())
    return failure("module instance is not mapped: " + ID);
  auto Symbol = I->Source->lookupSymbol(Name);
  if (!Symbol)
    return Symbol.takeError();
  if (!*Symbol)
    return std::optional<uint64_t>();
  const auto OriginalBase = I->Source->image().Base;
  if (!contains(OriginalBase, I->Size, (**Symbol).Address,
                std::max(uint64_t(1), (**Symbol).Size)))
    return failure("module symbol is outside the bound image extent");
  return std::optional(I->Base + ((**Symbol).Address - OriginalBase));
}
llvm::Expected<std::optional<ModuleExport>>
ModuleBindings::lookupExport(llvm::StringRef ID, llvm::StringRef Name) const {
  const auto I = find(ID);
  if (!I || !I->mapped())
    return failure("export query requires a mapped module instance");
  for (const auto &E : I->Exports)
    if (E.Name == Name)
      return std::optional(E);
  return std::optional<ModuleExport>();
}
llvm::Expected<std::optional<ModuleExport>>
ModuleBindings::lookupExport(llvm::StringRef ID, uint32_t Ordinal) const {
  const auto I = find(ID);
  if (!I || !I->mapped())
    return failure("export query requires a mapped module instance");
  for (const auto &E : I->Exports)
    if (E.Ordinal == Ordinal)
      return std::optional(E);
  return std::optional<ModuleExport>();
}
llvm::Expected<ModuleRequest> ModuleBindings::issue(ModuleRequest Request) {
  if (Busy || Pending || Sequence == UINT64_MAX)
    return failure("module resource request is already pending or unavailable");
  if (auto E = Request.Space->validateBacking(0, 0))
    return std::move(E);
  Request.Token = {Owner, Revision, ++Sequence};
  Pending = std::move(Request);
  return *Pending;
}
llvm::Expected<ModuleRequest> ModuleBindings::requestAddress(
    std::shared_ptr<AddressSpace> Space, uint64_t PC, uint64_t Address,
    uint64_t Size, ModuleAccess Access, unsigned RequiredPermissions) {
  const unsigned AccessBit = Access == ModuleAccess::Read    ? Read
                             : Access == ModuleAccess::Write ? Write
                                                             : Execute;
  if (!Space || !extent(Address, Size) ||
      (Access != ModuleAccess::Read && Access != ModuleAccess::Write &&
       Access != ModuleAccess::Fetch) ||
      (RequiredPermissions && (!(RequiredPermissions & AccessBit) ||
                               (RequiredPermissions & ~GuestPermissionMask))))
    return failure("invalid missing-address request");
  auto Layout = Space->mappings();
  if (!Layout)
    return Layout.takeError();
  for (const auto &M : *Layout)
    if (M.Device && overlaps(Address, Size, M.Address, M.Size))
      return failure("device access is not a missing module resource");
  auto Mapped = Space->canAccess(Address, Size, 0);
  if (!Mapped)
    return Mapped.takeError();
  if (*Mapped)
    return failure("mapped memory is not a missing module resource");
  ModuleRequest Request;
  Request.Space = std::move(Space);
  Request.PC = PC;
  Request.Address = Address;
  Request.Size = Size;
  Request.Access = Access;
  Request.RequiredPermissions =
      RequiredPermissions ? RequiredPermissions : AccessBit;
  for (const auto &[ID, I] : Instances) {
    const auto &B = I.Binding;
    if (B.Space == Request.Space && B.Base && B.Size &&
        contains(*B.Base, *B.Size, Address, Size)) {
      if (I.mapped())
        return failure(
            "access in a mapped module is not a missing module resource");
      Request.Kind = ModuleRequestKind::DeclaredModule;
      Request.InstanceID = ID;
      Request.ModuleID = B.ModuleID;
      break;
    }
  }
  return issue(std::move(Request));
}
llvm::Expected<ModuleRequest>
ModuleBindings::requestModule(llvm::StringRef ID) {
  const auto I = find(ID);
  if (!I || I->mapped())
    return failure("module request requires an unmapped declaration");
  ModuleRequest Request;
  Request.Kind = ModuleRequestKind::DeclaredModule;
  Request.Space = I->Binding.Space;
  Request.InstanceID = I->Binding.ID;
  Request.ModuleID = I->Binding.ModuleID;
  return issue(std::move(Request));
}
llvm::Expected<ModuleRequest>
ModuleBindings::requestDependency(std::shared_ptr<AddressSpace> Space,
                                  llvm::StringRef Dependency, uint64_t PC,
                                  std::optional<std::string> InstanceID) {
  if (!Space || !text(Dependency, 4096))
    return failure("invalid module dependency request");
  ModuleRequest Request;
  Request.Kind = ModuleRequestKind::MissingDependency;
  Request.Space = std::move(Space);
  Request.Dependency = Dependency.str();
  Request.PC = PC;
  if (InstanceID) {
    auto I = find(*InstanceID);
    if (!I || I->mapped() || I->Binding.Space != Request.Space)
      return failure(
          "dependency instance is not an unmapped declaration in this space");
    Request.InstanceID = *InstanceID;
    Request.ModuleID = I->Binding.ModuleID;
  }
  return issue(std::move(Request));
}
llvm::Error ModuleBindings::validateToken(ModuleRequestToken Token) const {
  if (Busy || !Pending || Token != Pending->Token || Token.Owner != Owner ||
      Token.Version != Revision)
    return failure("stale, consumed or foreign module request token");
  return llvm::Error::success();
}
llvm::Error ModuleBindings::cancel(ModuleRequestToken Token) {
  if (auto E = validateToken(Token))
    return E;
  Pending.reset();
  return llvm::Error::success();
}
llvm::Error ModuleBindings::supply(ModuleRequestToken Token,
                                   llvm::ArrayRef<ModuleBinding> Bindings,
                                   const ModuleMappingBuilder &Builder) {
  if (auto E = validateToken(Token))
    return E;
  if (auto E = materializeImpl(Bindings, Builder, &*Pending))
    return E;
  Pending.reset();
  return llvm::Error::success();
}
llvm::Error ModuleBindings::materialize(llvm::ArrayRef<ModuleBinding> Bindings,
                                        const ModuleMappingBuilder &Builder) {
  if (Pending)
    return failure("pending resources must be committed through supply");
  return materializeImpl(Bindings, Builder, nullptr);
}
llvm::Error
ModuleBindings::prepareEager(const std::shared_ptr<AddressSpace> &Space,
                             const ModuleMappingBuilder &Builder) {
  if (!Space || Busy || Pending)
    return failure("eager preparation requires an available address space");
  std::vector<ModuleBinding> Eager;
  for (const auto &[ID, I] : Instances)
    if (I.Binding.Space == Space && !I.mapped() &&
        I.Load == modules::LoadPolicy::Eager)
      Eager.push_back(I.Binding);
  if (Eager.empty())
    return llvm::Error::success();
  return materialize(Eager, Builder);
}
} // namespace neverd::emulation
