//===- LinuxDeviceMappings.cpp - Explicit shared device RAM snapshots ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxMemory.h"

#include <algorithm>

namespace neverd::emulation::linux_model {
std::optional<uint64_t> LinuxMemory::firstDeviceByte(uint64_t Address,
                                                     uint64_t End) const {
  if (Address >= End)
    return std::nullopt;
  for (const auto &[Begin, Limit] : ReadOnlyDeviceRanges)
    if (Begin < End && Limit > Address)
      return std::max(Address, Begin);
  return std::nullopt;
}

void LinuxMemory::retireDeviceRanges(uint64_t Address, uint64_t Size) {
  std::vector<std::pair<uint64_t, uint64_t>> Kept;
  const uint64_t End = Address + Size;
  for (const auto &[Begin, Limit] : ReadOnlyDeviceRanges) {
    if (Limit <= Address || Begin >= End) {
      Kept.emplace_back(Begin, Limit);
      continue;
    }
    if (Begin < Address)
      Kept.emplace_back(Begin, Address);
    if (Limit > End)
      Kept.emplace_back(End, Limit);
  }
  ReadOnlyDeviceRanges = std::move(Kept);
}

llvm::Expected<bool> LinuxMemory::mapDevicePages(
    uint64_t Address, uint64_t Size, unsigned Permissions,
    const LinuxDeviceMapping &Mapping, uint64_t Offset) {
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
        E = Space.writeBacking(Address + I * PageSize,
                               llvm::ArrayRef<uint8_t>(Mapping.Bytes)
                                   .slice(Offset + I * PageSize, PageSize));
    }
    if (E) {
      if (Mapped)
        E = llvm::joinErrors(std::move(E), Space.unmap(Address, Mapped));
      return std::move(E);
    }
  }
  for (size_t I = 0; I < Pages.size(); ++I)
    Cached[Offset / PageSize + I] = Pages[I];
  ReadOnlyDeviceRanges.emplace_back(Address, Address + Size);
  std::sort(ReadOnlyDeviceRanges.begin(), ReadOnlyDeviceRanges.end());
  return true;
}

llvm::Expected<std::optional<uint64_t>>
LinuxMemory::mapDevice(const ProcessServiceEvent &Event,
                       const LinuxDeviceOptions *Device,
                       ProcessResult &Result) {
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
  if (!Backing)
    return Unsupported();
  if (Device->AccessMode == 1 || (Protection & ~ProtRead))
    return std::optional<uint64_t>(uint64_t(0) - AccessDenied);
  auto Address = placement(Hint, *Size);
  if (!Address)
    return Address.takeError();
  if (!*Address)
    return std::optional<uint64_t>(uint64_t(0) - NoMemory);
  auto Mapped = mapDevicePages(**Address, *Size,
                               UserAccessible | (Protection ? Read : 0u),
                               *Backing, Offset - Backing->Offset);
  if (!Mapped)
    return Mapped.takeError();
  return std::optional<uint64_t>(*Mapped ? **Address : uint64_t(0) - NoMemory);
}
} // namespace neverd::emulation::linux_model
