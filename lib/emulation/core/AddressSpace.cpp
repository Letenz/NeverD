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
         !(Permissions & ~(Read | Write | Execute));
}
} // namespace
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
std::optional<BackendFaultKind>
AddressSpace::Impl::check(uint64_t Address, uint64_t Size,
                          unsigned Permissions) const {
  if (!Size)
    return std::nullopt;
  if (Size - 1 > UINT64_MAX - Address)
    return BackendFaultKind::InvalidMemoryRange;
  const uint64_t Last = (Address + Size - 1) & ~(memory::PageSize - 1);
  for (uint64_t VA = Address & ~(memory::PageSize - 1);;
       VA += memory::PageSize) {
    auto I = Pages.find(VA);
    if (I == Pages.end())
      return BackendFaultKind::UnmappedMemory;
    if ((I->second.Permissions & Permissions) != Permissions)
      return BackendFaultKind::Protection;
    if (VA == Last)
      return std::nullopt;
  }
}
llvm::Expected<bool> AddressSpace::canAccess(uint64_t Address, uint64_t Size,
                                             unsigned Permissions) const {
  std::unique_lock Lock(State->Memory->State->Mutex, std::try_to_lock);
  if (!Lock.owns_lock())
    return diagnostic::error(diagnostic::Running);
  if (Permissions & ~(Read | Write | Execute))
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
    std::memcpy(Backing, Bytes.data(), Count);
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
