//===- AddressSpace.cpp - Transactional guest virtual mappings ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/emulation/AddressSpace.h"

#include "ExecutionDiagnostics.h"
#include "MemoryLayout.h"
#include "MemoryStorage.h"

#include <algorithm>
#include <cstring>

namespace neverd::emulation {
namespace {
bool valid(uint64_t Address, uint64_t Size, unsigned Permissions) {
  return Size && !(Address % memory::PageSize) && !(Size % memory::PageSize) &&
         Size - 1 <= UINT64_MAX - Address &&
         !(Permissions & ~GuestPermissionMask);
}
} // namespace
struct AddressSpace::MappingTransaction::Impl {
  struct Patch {
    uint64_t Physical;
    std::vector<uint8_t> Bytes;
  };
  std::shared_ptr<AddressSpace> Space;
  std::unique_lock<std::recursive_mutex> Lock;
  std::map<uint64_t, AddressSpace::Impl::Page> Pages;
  std::vector<Patch> Patches;
  uint64_t Used;
  bool Committed = false;
  bool MetadataOnly = false;
  Impl(std::shared_ptr<AddressSpace> Space,
       std::unique_lock<std::recursive_mutex> Lock,
       std::map<uint64_t, AddressSpace::Impl::Page> Pages, uint64_t Used)
      : Space(std::move(Space)), Lock(std::move(Lock)), Pages(std::move(Pages)),
        Used(Used) {}
};
AddressSpace::MappingTransaction::MappingTransaction(
    std::unique_ptr<Impl> State)
    : State(std::move(State)) {}
AddressSpace::MappingTransaction::~MappingTransaction() {
  // Drop uncommitted reservations while the stopped lease is still held.
  State->Pages.clear();
  State->Space->State->Memory->State->Running = false;
}
llvm::Error AddressSpace::MappingTransaction::commit() {
  if (State->Committed)
    return diagnostic::error(diagnostic::InvalidMapping);
  if (State->MetadataOnly) {
    State->Committed = true;
    return llvm::Error::success();
  }
  auto &Target = *State->Space->State;
  auto &RAM = *Target.Memory->State;
  for (const auto &P : State->Patches) {
    std::memcpy(static_cast<uint8_t *>(RAM.Backing.base()) + P.Physical -
                    memory::ProjectionReserve,
                P.Bytes.data(), P.Bytes.size());
    RAM.invalidateReservations(P.Physical, P.Bytes.size());
  }
  Target.Pages.swap(State->Pages);
  Target.Used = State->Used;
  ++Target.Generation;
  State->Committed = true;
  return llvm::Error::success();
}
llvm::Expected<std::unique_ptr<AddressSpace::MappingTransaction>>
AddressSpace::prepareMappings(llvm::ArrayRef<InitializedMapping> Mappings,
                              llvm::ArrayRef<BackingPatch> Patches) {
  auto &RAM = *State->Memory->State;
  std::unique_lock Lock(RAM.Mutex, std::try_to_lock);
  if (!Lock.owns_lock() || RAM.Running)
    return diagnostic::error(diagnostic::Running);
  if (Mappings.empty() && Patches.empty())
    return diagnostic::error(diagnostic::InvalidMapping);
  auto Next = State->Pages;
  uint64_t Used = State->Used;
  // Validate every extent and collision before reserving physical capacity.
  for (const auto &M : Mappings) {
    if (!valid(M.Address, M.Bytes.size(), M.Permissions))
      return diagnostic::error(diagnostic::InvalidMapping);
    if (M.Bytes.size() > State->Limit - Used)
      return llvm::make_error<GuestMemoryLimitError>();
    for (uint64_t N = 0; N < M.Bytes.size(); N += memory::PageSize)
      if (!Next.emplace(M.Address + N, Impl::Page{}).second)
        return diagnostic::error(diagnostic::InvalidMapping);
    Used += M.Bytes.size();
  }
  for (const auto &M : Mappings) {
    auto Region = State->Memory->allocate(M.Bytes.size());
    if (!Region)
      return Region.takeError();
    std::memcpy(static_cast<uint8_t *>(RAM.Backing.base()) + (*Region)->Offset,
                M.Bytes.data(), M.Bytes.size());
    for (uint64_t N = 0; N < M.Bytes.size(); N += memory::PageSize)
      Next.at(M.Address + N) =
          Impl::Page{memory::ProjectionReserve + (*Region)->Offset + N,
                     M.Permissions,
                     *Region,
                     {}};
  }
  std::vector<MappingTransaction::Impl::Patch> PatchCopies;
  uint64_t PatchBytes = 0;
  for (const auto &P : Patches) {
    if (P.Bytes.empty() || P.Bytes.size() - 1 > UINT64_MAX - P.Address ||
        P.Bytes.size() > RAM.Limit - PatchBytes)
      return diagnostic::error(diagnostic::InvalidMapping);
    PatchBytes += P.Bytes.size();
    auto Bytes = P.Bytes;
    uint64_t A = P.Address;
    while (!Bytes.empty()) {
      const uint64_t Offset = A % memory::PageSize;
      const auto Page = Next.find(A - Offset);
      if (Page == Next.end() || Page->second.IO)
        return diagnostic::error(diagnostic::MemoryAccess);
      const uint64_t N =
          std::min(uint64_t(Bytes.size()), memory::PageSize - Offset);
      PatchCopies.push_back(
          {Page->second.Physical + Offset, {Bytes.begin(), Bytes.begin() + N}});
      Bytes = Bytes.drop_front(N);
      if (!Bytes.empty())
        A += N;
    }
  }
  std::sort(
      PatchCopies.begin(), PatchCopies.end(),
      [](const auto &A, const auto &B) { return A.Physical < B.Physical; });
  for (size_t I = 1; I < PatchCopies.size(); ++I)
    if (PatchCopies[I].Physical - PatchCopies[I - 1].Physical <
        PatchCopies[I - 1].Bytes.size())
      return diagnostic::error(diagnostic::InvalidMapping);
  auto Prepared = std::unique_ptr<MappingTransaction>(
      new MappingTransaction(std::make_unique<MappingTransaction::Impl>(
          shared_from_this(), std::move(Lock), std::move(Next), Used)));
  Prepared->State->Patches = std::move(PatchCopies);
  // Prevent recursive execution/mutation until RAM and its consumer metadata
  // have both been published. Other host threads also fail the held lock.
  RAM.Running = true;
  return Prepared;
}
llvm::Expected<std::unique_ptr<AddressSpace::MappingTransaction>>
AddressSpace::prepareMetadataPublication() {
  auto &RAM = *State->Memory->State;
  std::unique_lock Lock(RAM.Mutex, std::try_to_lock);
  if (!Lock.owns_lock() || RAM.Running)
    return diagnostic::error(diagnostic::Running);
  auto Prepared = std::unique_ptr<MappingTransaction>(
      new MappingTransaction(std::make_unique<MappingTransaction::Impl>(
          shared_from_this(), std::move(Lock), std::map<uint64_t, Impl::Page>{},
          0)));
  Prepared->State->MetadataOnly = true;
  RAM.Running = true;
  return Prepared;
}
AddressSpace::AddressSpace(std::unique_ptr<Impl> State)
    : State(std::move(State)) {}
AddressSpace::~AddressSpace() = default;
llvm::Expected<std::shared_ptr<AddressSpace>>
AddressSpace::create(std::shared_ptr<PhysicalMemory> Memory,
                     uint64_t MappingLimit) {
  if (!Memory || !MappingLimit || MappingLimit > memory::MaxRAM)
    return diagnostic::error(diagnostic::AddressSpace);
  auto State = std::make_unique<Impl>();
  State->Memory = std::move(Memory);
  State->Limit = MappingLimit;
  return std::shared_ptr<AddressSpace>(new AddressSpace(std::move(State)));
}
std::shared_ptr<AddressSpace> AddressSpace::addressSpace() const {
  return std::const_pointer_cast<AddressSpace>(shared_from_this());
}
std::shared_ptr<const void> AddressSpace::identity() const {
  return State->Identity;
}
llvm::Expected<MemoryView> AddressSpace::allocateBacking(uint64_t Size) {
  auto Region = State->Memory->allocate(Size);
  if (!Region)
    return Region.takeError();
  return MemoryView(State->Memory, State->Identity, Size,
                    {{std::move(*Region), 0, Size}});
}
llvm::Expected<MemoryView> AddressSpace::pinBacking(uint64_t Address,
                                                    uint64_t Size) const {
  auto &RAM = *State->Memory->State;
  std::unique_lock Lock(RAM.Mutex, std::try_to_lock);
  if (!Lock.owns_lock())
    return diagnostic::error(diagnostic::Running);
  if (State->check(Address, Size, 0) || State->overlapsDevice(Address, Size))
    return diagnostic::error(diagnostic::MemoryAccess);
  std::vector<MemorySlice> Slices;
  const uint64_t Total = Size;
  while (Size) {
    const uint64_t InPage = Address % memory::PageSize;
    const auto &Page = State->Pages.at(Address - InPage);
    const uint64_t Offset = Page.Physical - memory::ProjectionReserve -
                            Page.Region->Offset + InPage;
    const uint64_t Count = std::min(Size, memory::PageSize - InPage);
    if (!Slices.empty() && Slices.back().Region == Page.Region &&
        Slices.back().Offset + Slices.back().Size == Offset)
      Slices.back().Size += Count;
    else
      Slices.push_back({Page.Region, Offset, Count});
    Address += Count;
    Size -= Count;
  }
  return MemoryView(State->Memory, State->Identity, Total, std::move(Slices));
}
llvm::Error AddressSpace::validatePinned(const MemoryView &View,
                                         uint64_t Offset, uint64_t Size) const {
  if (View.physicalMemory() != State->Memory)
    return diagnostic::error(diagnostic::MemoryOwner);
  return View.validateAccess(Offset, Size);
}
llvm::Error AddressSpace::readPinned(const MemoryView &View, uint64_t Offset,
                                     llvm::MutableArrayRef<uint8_t> Bytes) {
  if (View.physicalMemory() != State->Memory)
    return diagnostic::error(diagnostic::MemoryOwner);
  return View.read(Offset, Bytes);
}
llvm::Error AddressSpace::writePinned(const MemoryView &View, uint64_t Offset,
                                      llvm::ArrayRef<uint8_t> Bytes) {
  if (View.physicalMemory() != State->Memory)
    return diagnostic::error(diagnostic::MemoryOwner);
  return View.write(Offset, Bytes);
}
std::shared_ptr<PhysicalMemory> AddressSpace::physicalMemory() const {
  return State->Memory;
}
uint64_t AddressSpace::mappedBytes() const { return State->Used.load(); }
uint64_t AddressSpace::mappingGeneration() const {
  return State->Generation.load();
}
llvm::Expected<std::vector<AddressMapping>> AddressSpace::mappings() const {
  std::unique_lock Lock(State->Memory->State->Mutex, std::try_to_lock);
  if (!Lock.owns_lock())
    return diagnostic::error(diagnostic::Running);
  std::vector<AddressMapping> Result;
  for (const auto &[Address, Page] : State->Pages) {
    if (!Result.empty()) {
      auto &Previous = Result.back();
      if (Address - Previous.Address == Previous.Size &&
          Page.Permissions == Previous.Permissions &&
          bool(Page.IO) == Previous.Device) {
        Previous.Size += memory::PageSize;
        continue;
      }
    }
    Result.push_back(
        {Address, memory::PageSize, Page.Permissions, bool(Page.IO)});
  }
  return Result;
}
llvm::Error AddressSpace::mapRegion(uint64_t Address,
                                    std::shared_ptr<MemoryRegion> Region,
                                    uint64_t Offset, uint64_t Size,
                                    unsigned Permissions) {
  auto &RAM = *State->Memory->State;
  std::unique_lock Lock(RAM.Mutex, std::try_to_lock);
  if (!Lock.owns_lock() || RAM.Running)
    return diagnostic::error(diagnostic::Running);
  if (!valid(Address, Size, Permissions))
    return diagnostic::error(diagnostic::InvalidMapping);
  if (!Region || Offset % memory::PageSize || Offset > Region->Size ||
      Size > Region->Size - Offset)
    return diagnostic::error(diagnostic::MemoryRegion);
  if (Region->Owner != State->Memory)
    return diagnostic::error(diagnostic::MemoryOwner);
  if (Size > State->Limit - State->Used)
    return llvm::make_error<GuestMemoryLimitError>();
  for (uint64_t N = 0; N < Size; N += memory::PageSize)
    if (State->Pages.count(Address + N))
      return diagnostic::error(diagnostic::InvalidMapping);
  for (uint64_t N = 0; N < Size; N += memory::PageSize)
    State->Pages.emplace(
        Address + N,
        Impl::Page{memory::ProjectionReserve + Region->Offset + Offset + N,
                   Permissions,
                   Region,
                   {}});
  State->Used += Size;
  ++State->Generation;
  return llvm::Error::success();
}
llvm::Error AddressSpace::map(uint64_t Address, uint64_t Size,
                              unsigned Permissions) {
  auto &RAM = *State->Memory->State;
  std::unique_lock Lock(RAM.Mutex, std::try_to_lock);
  if (!Lock.owns_lock() || RAM.Running)
    return diagnostic::error(diagnostic::Running);
  if (!valid(Address, Size, Permissions))
    return diagnostic::error(diagnostic::InvalidMapping);
  if (Size > State->Limit - State->Used)
    return llvm::make_error<GuestMemoryLimitError>();
  for (uint64_t N = 0; N < Size; N += memory::PageSize)
    if (State->Pages.count(Address + N))
      return diagnostic::error(diagnostic::InvalidMapping);
  auto Region = State->Memory->allocate(Size);
  if (!Region)
    return Region.takeError();
  return mapRegion(Address, std::move(*Region), 0, Size, Permissions);
}
llvm::Error AddressSpace::unmap(uint64_t Address, uint64_t Size) {
  auto &RAM = *State->Memory->State;
  std::unique_lock Lock(RAM.Mutex, std::try_to_lock);
  if (!Lock.owns_lock() || RAM.Running)
    return diagnostic::error(diagnostic::Running);
  if (!valid(Address, Size, 0) || State->check(Address, Size, 0) ||
      State->overlapsDevice(Address, Size))
    return diagnostic::error(diagnostic::InvalidMapping);
  std::map<uint64_t, uint64_t> Aliases;
  for (const auto &[Base, Length] : State->Aliases) {
    // Subtraction-based intersection avoids an end-address overflow.
    const bool Intersects =
        Address <= Base ? Base - Address < Size : Address - Base < Length;
    if (!Intersects) {
      Aliases.emplace(Base, Length);
      continue;
    }
    if (Base < Address)
      Aliases.emplace(Base, Address - Base);
    const uint64_t Removed =
        Address <= Base
            ? std::min(Length, Size - (Base - Address))
            : Address - Base + std::min(Size, Length - (Address - Base));
    if (Removed < Length)
      Aliases.emplace(Base + Removed, Length - Removed);
  }
  for (uint64_t N = 0; N < Size; N += memory::PageSize)
    State->Pages.erase(Address + N);
  State->Aliases.swap(Aliases);
  State->Used -= Size;
  ++State->Generation;
  return llvm::Error::success();
}
llvm::Error AddressSpace::mapAlias(uint64_t Address, uint64_t Source,
                                   uint64_t Size, unsigned Permissions) {
  return replaceAliases({}, {{Address, Source, Size, Permissions}});
}
llvm::Error AddressSpace::unmapAlias(uint64_t Address, uint64_t Size) {
  return replaceAliases({{Address, Size}}, {});
}
llvm::Error
AddressSpace::replaceAliases(llvm::ArrayRef<GuestAliasRange> Remove,
                             llvm::ArrayRef<GuestAliasMapping> Add) {
  auto &RAM = *State->Memory->State;
  std::unique_lock Lock(RAM.Mutex, std::try_to_lock);
  if (!Lock.owns_lock() || RAM.Running)
    return diagnostic::error(diagnostic::Running);
  auto Next = State->Pages;
  auto Aliases = State->Aliases;
  uint64_t Used = State->Used;
  for (const auto &R : Remove) {
    auto I = Aliases.find(R.Address);
    if (I == Aliases.end() || I->second != R.Size)
      return diagnostic::error(diagnostic::InvalidMapping);
    for (uint64_t N = 0; N < R.Size; N += memory::PageSize)
      Next.erase(R.Address + N);
    Aliases.erase(I);
    Used -= R.Size;
  }
  const auto Sources = Next;
  for (const auto &R : Add) {
    if (!valid(R.Address, R.Size, R.Permissions) ||
        !valid(R.Source, R.Size, R.Permissions))
      return diagnostic::error(diagnostic::InvalidMapping);
    if (R.Size > State->Limit - Used)
      return llvm::make_error<GuestMemoryLimitError>();
    for (uint64_t N = 0; N < R.Size; N += memory::PageSize) {
      auto I = Sources.find(R.Source + N);
      if (I == Sources.end() || I->second.IO || Next.count(R.Address + N))
        return diagnostic::error(diagnostic::InvalidMapping);
      auto Page = I->second;
      Page.Permissions = R.Permissions;
      Next.emplace(R.Address + N, std::move(Page));
    }
    Aliases.emplace(R.Address, R.Size);
    Used += R.Size;
  }
  State->Pages.swap(Next);
  State->Aliases.swap(Aliases);
  State->Used = Used;
  ++State->Generation;
  return llvm::Error::success();
}
llvm::Error AddressSpace::protect(uint64_t Address, uint64_t Size,
                                  unsigned Permissions) {
  auto &RAM = *State->Memory->State;
  std::unique_lock Lock(RAM.Mutex, std::try_to_lock);
  if (!Lock.owns_lock() || RAM.Running)
    return diagnostic::error(diagnostic::Running);
  if (!valid(Address, Size, Permissions) || State->check(Address, Size, 0))
    return diagnostic::error(diagnostic::InvalidMapping);
  if (State->overlapsDevice(Address, Size))
    return diagnostic::error(diagnostic::DeviceProtection);
  for (uint64_t N = 0; N < Size; N += memory::PageSize)
    State->Pages.at(Address + N).Permissions = Permissions;
  ++State->Generation;
  return llvm::Error::success();
}
bool AddressSpace::Impl::overlapsDevice(uint64_t Address, uint64_t Size) const {
  if (!Size)
    return false;
  for (const auto &[Base, IO] : Devices)
    if (Address <= Base ? Base - Address < Size : Address - Base < IO->Size)
      return true;
  return false;
}
llvm::Error AddressSpace::mapMMIO(uint64_t Address, uint64_t Size,
                                  GuestMMIOCallbacks Callbacks) {
  auto &RAM = *State->Memory->State;
  std::unique_lock Lock(RAM.Mutex, std::try_to_lock);
  if (!Lock.owns_lock() || RAM.Running)
    return diagnostic::error(diagnostic::Running);
  if (!valid(Address, Size, Read | Write))
    return diagnostic::error(diagnostic::InvalidMapping);
  if (!Callbacks.Validate || !Callbacks.Read || !Callbacks.Write)
    return diagnostic::error(diagnostic::DeviceCallbacks);
  if (Size > State->Limit - State->Used)
    return llvm::make_error<GuestMemoryLimitError>();
  for (uint64_t N = 0; N < Size; N += memory::PageSize)
    if (State->Pages.count(Address + N))
      return diagnostic::error(diagnostic::InvalidMapping);
  auto IO = std::make_shared<Impl::Device>(
      Impl::Device{Address, Size, std::move(Callbacks)});
  for (uint64_t N = 0; N < Size; N += memory::PageSize)
    State->Pages.emplace(Address + N, Impl::Page{0, Read | Write, {}, IO});
  State->Devices.emplace(Address, std::move(IO));
  State->Used += Size;
  ++State->Generation;
  return llvm::Error::success();
}
llvm::Error AddressSpace::unmapMMIO(uint64_t Address, uint64_t Size) {
  auto &RAM = *State->Memory->State;
  std::unique_lock Lock(RAM.Mutex, std::try_to_lock);
  if (!Lock.owns_lock() || RAM.Running)
    return diagnostic::error(diagnostic::Running);
  auto I = State->Devices.find(Address);
  if (I == State->Devices.end() || I->second->Size != Size)
    return diagnostic::error(diagnostic::DeviceRetirement);
  // Stale engine projections may retain the descriptor, but the successful
  // logical retirement releases callback ownership at this stopped boundary.
  I->second->Callbacks = {};
  for (uint64_t N = 0; N < Size; N += memory::PageSize)
    State->Pages.erase(Address + N);
  State->Devices.erase(I);
  State->Used -= Size;
  ++State->Generation;
  return llvm::Error::success();
}
std::optional<MemoryAccessFailure>
AddressSpace::Impl::firstAccessFailure(uint64_t Address, uint64_t Size,
                                       unsigned Permissions) const {
  if (!Size)
    return std::nullopt;
  if (Size - 1 > UINT64_MAX - Address)
    return MemoryAccessFailure{BackendFaultKind::InvalidMemoryRange, Address,
                               Size};
  const uint64_t Last = (Address + Size - 1) & ~(memory::PageSize - 1);
  for (uint64_t VA = Address & ~(memory::PageSize - 1);;
       VA += memory::PageSize) {
    auto I = Pages.find(VA);
    const uint64_t First = std::max(Address, VA);
    const uint64_t Count = std::min(
        Size - (First - Address), memory::PageSize - First % memory::PageSize);
    if (I == Pages.end())
      return MemoryAccessFailure{BackendFaultKind::UnmappedMemory, First,
                                 Count};
    if ((I->second.Permissions & Permissions) != Permissions)
      return MemoryAccessFailure{BackendFaultKind::Protection, First, Count};
    if (VA == Last)
      return std::nullopt;
  }
}
std::optional<BackendFaultKind>
AddressSpace::Impl::check(uint64_t Address, uint64_t Size,
                          unsigned Permissions) const {
  if (auto Failure = firstAccessFailure(Address, Size, Permissions))
    return Failure->Kind;
  return std::nullopt;
}
llvm::Expected<bool> AddressSpace::canAccess(uint64_t Address, uint64_t Size,
                                             unsigned Permissions) const {
  std::unique_lock Lock(State->Memory->State->Mutex, std::try_to_lock);
  if (!Lock.owns_lock())
    return diagnostic::error(diagnostic::Running);
  if (Permissions & ~GuestPermissionMask)
    return diagnostic::error(diagnostic::InvalidMapping);
  return !State->check(Address, Size, Permissions) &&
         !State->overlapsDevice(Address, Size);
}
llvm::Error
AddressSpace::readWithPermissions(uint64_t Address,
                                  llvm::MutableArrayRef<uint8_t> Bytes,
                                  unsigned Permissions) const {
  auto &RAM = *State->Memory->State;
  std::unique_lock Lock(RAM.Mutex, std::try_to_lock);
  if (!Lock.owns_lock())
    return diagnostic::error(diagnostic::Running);
  if (State->check(Address, Bytes.size(), Permissions) ||
      State->overlapsDevice(Address, Bytes.size()))
    return diagnostic::error(diagnostic::MemoryAccess);
  while (!Bytes.empty()) {
    const uint64_t Offset = Address % memory::PageSize;
    const auto &Page = State->Pages.at(Address - Offset);
    const uint64_t Count =
        std::min<uint64_t>(Bytes.size(), memory::PageSize - Offset);
    auto *Backing = static_cast<const uint8_t *>(RAM.Backing.base()) +
                    Page.Physical - memory::ProjectionReserve + Offset;
    std::memcpy(Bytes.data(), Backing, Count);
    Bytes = Bytes.drop_front(Count);
    Address += Count;
  }
  return llvm::Error::success();
}
llvm::Error AddressSpace::writeWithPermissions(uint64_t Address,
                                               llvm::ArrayRef<uint8_t> Bytes,
                                               unsigned Permissions) {
  auto &RAM = *State->Memory->State;
  std::unique_lock Lock(RAM.Mutex, std::try_to_lock);
  if (!Lock.owns_lock() || RAM.Running)
    return diagnostic::error(diagnostic::Running);
  if (State->check(Address, Bytes.size(), Permissions) ||
      State->overlapsDevice(Address, Bytes.size()))
    return diagnostic::error(diagnostic::MemoryAccess);
  while (!Bytes.empty()) {
    const uint64_t Offset = Address % memory::PageSize;
    const auto &Page = State->Pages.at(Address - Offset);
    const uint64_t Count =
        std::min<uint64_t>(Bytes.size(), memory::PageSize - Offset);
    auto *Backing = static_cast<uint8_t *>(RAM.Backing.base()) + Page.Physical -
                    memory::ProjectionReserve + Offset;
    RAM.invalidateReservations(Page.Physical + Offset, Count);
    std::memcpy(Backing, Bytes.data(), Count);
    RAM.recordInitialization(Page.Physical + Offset, Count);
    Bytes = Bytes.drop_front(Count);
    Address += Count;
  }
  return llvm::Error::success();
}
llvm::Error AddressSpace::read(uint64_t Address,
                               llvm::MutableArrayRef<uint8_t> Bytes) {
  return readWithPermissions(Address, Bytes, Read);
}
llvm::Error AddressSpace::write(uint64_t Address,
                                llvm::ArrayRef<uint8_t> Bytes) {
  return writeWithPermissions(Address, Bytes, Write);
}
llvm::Error AddressSpace::validateBacking(uint64_t Address,
                                          uint64_t Size) const {
  auto &RAM = *State->Memory->State;
  std::unique_lock Lock(RAM.Mutex, std::try_to_lock);
  if (!Lock.owns_lock() || RAM.Running)
    return diagnostic::error(diagnostic::Running);
  if (State->check(Address, Size, 0) || State->overlapsDevice(Address, Size))
    return diagnostic::error(diagnostic::MemoryAccess);
  return llvm::Error::success();
}
llvm::Error AddressSpace::readBacking(uint64_t Address,
                                      llvm::MutableArrayRef<uint8_t> Bytes) {
  auto &RAM = *State->Memory->State;
  std::unique_lock Lock(RAM.Mutex, std::try_to_lock);
  if (!Lock.owns_lock() || RAM.Running)
    return diagnostic::error(diagnostic::Running);
  return readWithPermissions(Address, Bytes, 0);
}
llvm::Error AddressSpace::writeBacking(uint64_t Address,
                                       llvm::ArrayRef<uint8_t> Bytes) {
  return writeWithPermissions(Address, Bytes, 0);
}
llvm::Error
AddressSpace::snapshotBacking(uint64_t Address,
                              llvm::MutableArrayRef<uint8_t> Bytes) {
  return readBacking(Address, Bytes);
}
} // namespace neverd::emulation
