//===- LinuxDeviceMappings.cpp - Explicit shared device RAM snapshots ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxMemory.h"

#include "neverd/emulation/Limits.h"

#include <algorithm>

namespace neverd::emulation::linux_model {
std::optional<uint64_t>
LinuxMemory::binderAddress(const LinuxDeviceState &Device, uint64_t Offset,
                           uint64_t Size) const {
  for (const auto &Alias : ReadOnlyDeviceRanges)
    if (Alias.Owner.get() == &Device &&
        Alias.Mapping == &Device.BinderMapping && Offset >= Alias.Offset &&
        Offset - Alias.Offset <= Alias.End - Alias.Begin &&
        Size <= Alias.End - Alias.Begin - (Offset - Alias.Offset))
      return Alias.Begin + Offset - Alias.Offset;
  return std::nullopt;
}
std::optional<uint64_t> LinuxMemory::firstDeviceByte(uint64_t Address,
                                                     uint64_t End) const {
  if (Address >= End)
    return std::nullopt;
  for (const auto &Alias : ReadOnlyDeviceRanges)
    if (Alias.Begin < End && Alias.End > Address)
      return std::max(Address, Alias.Begin);
  return std::nullopt;
}

void LinuxMemory::retireDeviceRanges(uint64_t Address, uint64_t Size) {
  std::vector<DeviceAlias> Kept;
  const uint64_t End = Address + Size;
  for (const auto &Alias : ReadOnlyDeviceRanges) {
    if (Alias.End <= Address || Alias.Begin >= End) {
      Kept.push_back(Alias);
      continue;
    }
    if (Alias.Begin < Address)
      Kept.push_back(
          {Alias.Begin, Address, Alias.Offset, Alias.Mapping, Alias.Owner});
    if (Alias.End > End)
      Kept.push_back({End, Alias.End, Alias.Offset + End - Alias.Begin,
                      Alias.Mapping, Alias.Owner});
  }
  ReadOnlyDeviceRanges = std::move(Kept);
}

bool LinuxMemory::canRetireDevices(uint64_t Address, uint64_t Size) const {
  using Reference = std::shared_ptr<LinuxDeviceState>;
  std::map<const LinuxDeviceState *, std::pair<const Reference *, uint64_t>>
      Dropped;
  for (const auto &Alias : ReadOnlyDeviceRanges)
    if (Alias.Owner && Address <= Alias.Begin && Alias.End <= Address + Size) {
      auto &Entry = Dropped[Alias.Owner.get()];
      Entry.first = &Alias.Owner;
      ++Entry.second;
    }
  for (const auto &[Device, Entry] : Dropped)
    if (uint64_t(Entry.first->use_count()) == Entry.second) {
      if (Device->hasPendingBinderWork())
        return false;
      for (uint32_t ID : Device->Contexts)
        if (!contextIdle(*Device, ID))
          return false;
    }
  return true;
}

llvm::ArrayRef<uint8_t>
LinuxMemory::deviceBytes(const LinuxDeviceMapping &Mapping) const {
  const auto I = DeviceStates.find(&Mapping);
  return I == DeviceStates.end() ? llvm::ArrayRef<uint8_t>(Mapping.Bytes)
                                 : llvm::ArrayRef<uint8_t>(I->second.Bytes);
}

llvm::Error LinuxMemory::writeDeviceBytes(const LinuxDeviceMapping &Mapping,
                                          uint64_t Offset,
                                          llvm::ArrayRef<uint8_t> Bytes) {
  if (Offset > Mapping.Bytes.size() ||
      Bytes.size() > Mapping.Bytes.size() - Offset)
    return failure(DeviceOptions);
  auto [I, Fresh] = DeviceStates.try_emplace(&Mapping);
  if (Fresh)
    I->second.Bytes = Mapping.Bytes;
  // One write per physical page is sufficient for every alias, including
  // PROT_NONE aliases. Device writes use the stopped RAM owner, never guest
  // permission changes, virtual address reuse or a saved host pointer.
  for (uint64_t Done = 0; Done < Bytes.size();) {
    const uint64_t Position = Offset + Done;
    const uint64_t Count =
        std::min<uint64_t>(Bytes.size() - Done, PageSize - Position % PageSize);
    for (const auto &Alias : ReadOnlyDeviceRanges) {
      if (Alias.Mapping != &Mapping || Position < Alias.Offset ||
          Position - Alias.Offset >= Alias.End - Alias.Begin)
        continue;
      if (auto E = Space.writeBacking(Alias.Begin + Position - Alias.Offset,
                                      Bytes.slice(Done, Count)))
        return E;
      break;
    }
    Done += Count;
  }
  std::copy(Bytes.begin(), Bytes.end(), I->second.Bytes.begin() + Offset);
  return llvm::Error::success();
}

llvm::Error LinuxMemory::synchronizeDevices(
    uint64_t Elapsed, const std::optional<LinuxFileOptions> &Options) {
  if (!Options || LastDeviceTime == Elapsed)
    return llvm::Error::success();
  if (LastDeviceTime && Elapsed < *LastDeviceTime)
    return failure(DeviceOptions);
  for (const auto &[Path, Device] : Options->Devices) {
    for (const auto &Mapping : Device.ReadOnlyMappings) {
      if (Mapping.Updates.empty())
        continue;
      auto [I, Fresh] = DeviceStates.try_emplace(&Mapping);
      if (Fresh)
        I->second.Bytes = Mapping.Bytes;
      auto &Next = I->second.NextUpdate;
      while (Next < Mapping.Updates.size() &&
             Mapping.Updates[Next].AfterNanoseconds <= Elapsed) {
        const auto &Update = Mapping.Updates[Next];
        if (auto E = writeDeviceBytes(Mapping, Update.Offset, Update.Bytes))
          return E;
        ++Next;
      }
    }
  }
  LastDeviceTime = Elapsed;
  return llvm::Error::success();
}

llvm::Expected<bool>
LinuxMemory::mapDevicePages(uint64_t Address, uint64_t Size,
                            unsigned Permissions,
                            const LinuxDeviceMapping &Mapping, uint64_t Offset,
                            std::shared_ptr<LinuxDeviceState> Owner) {
  auto RAM = Space.physicalMemory();
  if (Space.mappedBytes() > Limit || Size > Limit - Space.mappedBytes())
    return false;
  auto &Cached = DevicePages[&Mapping];
  Cached.resize(Mapping.Bytes.size() / PageSize);
  std::vector<std::shared_ptr<MemoryRegion>> Pages;
  std::vector<bool> Fresh;
  uint64_t Needed = 0;
  for (uint64_t I = 0; I < Size; I += PageSize) {
    auto Page = Cached[(Offset + I) / PageSize].lock();
    Fresh.push_back(!Page);
    if (!Page)
      Needed += PageSize;
    Pages.push_back(std::move(Page));
  }
  if (RAM->allocatedBytes() > Limit || Needed > Limit - RAM->allocatedBytes())
    return false;
  // Allocate all missing pages before publishing any mapping. Cache entries
  // are weak: only live aliases/observers pin physical memory after munmap.
  for (size_t I = 0; I < Pages.size(); ++I) {
    if (!Fresh[I])
      continue;
    auto Page = RAM->allocate(PageSize);
    if (!Page) {
      auto E = Page.takeError();
      if (!E.isA<GuestMemoryLimitError>())
        return std::move(E);
      llvm::consumeError(std::move(E));
      return false;
    }
    Pages[I] = std::move(*Page);
  }
  uint64_t Mapped = 0;
  for (size_t I = 0; I < Pages.size(); ++I) {
    auto E =
        Space.mapRegion(Address + Mapped, Pages[I], 0, PageSize, Permissions);
    if (!E) {
      Mapped += PageSize;
      if (Fresh[I])
        E = Space.writeBacking(
            Address + I * PageSize,
            deviceBytes(Mapping).slice(Offset + I * PageSize, PageSize));
    }
    if (E) {
      if (Mapped)
        E = llvm::joinErrors(std::move(E), Space.unmap(Address, Mapped));
      return std::move(E);
    }
  }
  for (size_t I = 0; I < Pages.size(); ++I)
    Cached[Offset / PageSize + I] = Pages[I];
  ReadOnlyDeviceRanges.push_back(
      {Address, Address + Size, Offset, &Mapping, std::move(Owner)});
  std::sort(ReadOnlyDeviceRanges.begin(), ReadOnlyDeviceRanges.end(),
            [](const DeviceAlias &A, const DeviceAlias &B) {
              return A.Begin < B.Begin;
            });
  return true;
}

llvm::Expected<std::optional<uint64_t>>
LinuxMemory::mapDevice(const ProcessServiceEvent &Event,
                       const LinuxDeviceOptions *Device, ProcessResult &Result,
                       std::shared_ptr<LinuxDeviceState> Owner) {
  auto Unsupported = [&]() -> std::optional<uint64_t> {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = DeviceMappingMissing;
    return std::nullopt;
  };
  const auto [Hint, Length, Protection, Flags, FD, Offset] = Event.Arguments;
  if (!Length || Offset % PageSize)
    return std::optional<uint64_t>(uint64_t(0) - InvalidArgument);
  auto Size = roundSize(Length);
  if (!Size || *Size > Limit)
    return std::optional<uint64_t>(uint64_t(0) - NoMemory);
  if (Device && Device->Binder && Owner) {
    if (Offset || PageSize != DeviceMappingPageSize ||
        *Size >
            ::neverd::limits::emulation::kLinuxDeviceMappingsBinderArenaLimit ||
        (Flags != 2 && Flags != (2 | 0x4000)) || Protection != ProtRead)
      return Unsupported();
    if (!Owner->BinderMapping.Bytes.empty())
      return std::optional<uint64_t>(uint64_t(0) - 16); // EBUSY
    auto Address = placement(Hint, *Size);
    if (!Address)
      return Address.takeError();
    if (!*Address)
      return std::optional<uint64_t>(uint64_t(0) - NoMemory);
    auto &Mapping = Owner->BinderMapping;
    Mapping.Bytes.resize(*Size);
    // A destroyed open may have occupied the same host address as this owner.
    DeviceStates.erase(&Mapping);
    DevicePages.erase(&Mapping);
    auto Mapped = mapDevicePages(**Address, *Size, UserAccessible | Read,
                                 Mapping, 0, Owner);
    if (!Mapped)
      return Mapped.takeError();
    if (!*Mapped)
      Mapping.Bytes.clear();
    return std::optional<uint64_t>(*Mapped ? **Address
                                           : uint64_t(0) - NoMemory);
  }
  if (!Device || Flags != MapShared || PageSize != DeviceMappingPageSize ||
      (Protection & ~(ProtRead | ProtWrite | ProtExecute)))
    return Unsupported();
  const LinuxDeviceMapping *Backing = nullptr;
  for (const auto &Mapping : Device->ReadOnlyMappings)
    if (Offset >= Mapping.Offset &&
        Offset - Mapping.Offset <= Mapping.Bytes.size() &&
        *Size <= Mapping.Bytes.size() - (Offset - Mapping.Offset)) {
      Backing = &Mapping;
      break;
    }
  if (!Backing) {
    if (Device->KGSL && Device->KGSL->MemoryObjects && Owner && Owner->GPU)
      return mapGPUObject(Event, std::move(Owner), Result);
    return Unsupported();
  }
  if (Device->AccessMode == 1 || (Protection & ~ProtRead))
    return std::optional<uint64_t>(uint64_t(0) - AccessDenied);
  auto Address = placement(Hint, *Size);
  if (!Address)
    return Address.takeError();
  if (!*Address)
    return std::optional<uint64_t>(uint64_t(0) - NoMemory);
  auto Mapped = mapDevicePages(
      **Address, *Size, UserAccessible | (Protection ? Read : 0u), *Backing,
      Offset - Backing->Offset, std::move(Owner));
  if (!Mapped)
    return Mapped.takeError();
  return std::optional<uint64_t>(*Mapped ? **Address : uint64_t(0) - NoMemory);
}
} // namespace neverd::emulation::linux_model
