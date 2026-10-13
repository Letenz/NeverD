//===- LinuxDeviceFiles.cpp - Explicit descriptor-owned transactions ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Applies bounded caller-supplied device observations through the shared
/// Linux descriptor table. No request encoding implies a driver ABI or reply.
//===----------------------------------------------------------------------===//
#include "LinuxFiles.h"

#include "llvm/Support/FormatVariadic.h"

#include <algorithm>

namespace neverd::emulation::linux_model {
llvm::Error validateDeviceOptions(const LinuxDeviceOptions &Options,
                                  uint64_t &Bytes, uint64_t &Transactions) {
  if (Options.AccessMode > 2 || Options.OpenErrno > 4095 ||
      Options.Ioctls.size() > DeviceIoctlLimit - Transactions)
    return failure(DeviceOptions);
  Transactions += Options.Ioctls.size();
  if (Options.ReadOnlyMappings.size() > DeviceMappingLimit)
    return failure(DeviceOptions);
  std::vector<std::pair<uint64_t, uint64_t>> Ranges;
  for (const auto &Mapping : Options.ReadOnlyMappings) {
    const uint64_t Size = Mapping.Bytes.size();
    // Both supported LP64 Linux profiles use 4 KiB pages. Complete supplied
    // pages avoid invented trailing bytes or device-specific SIGBUS behavior.
    if (!Size || Size % DeviceMappingPageSize ||
        Mapping.Offset % DeviceMappingPageSize || Mapping.Offset > INT64_MAX ||
        Size > uint64_t(INT64_MAX) - Mapping.Offset)
      return failure(DeviceOptions);
    if (Size > FileByteLimit - Bytes)
      return failure(FileOptionsLimit);
    Bytes += Size;
    Ranges.emplace_back(Mapping.Offset, Mapping.Offset + Size);
  }
  std::sort(Ranges.begin(), Ranges.end());
  for (size_t I = 1; I < Ranges.size(); ++I)
    if (Ranges[I].first < Ranges[I - 1].second)
      return failure(DeviceOptions);
  for (const auto &Call : Options.Ioctls) {
    if (Call.Result < -4095 || Call.Memory.size() > DeviceMemoryLimit)
      return failure(DeviceOptions);
    for (const auto &Memory : Call.Memory) {
      const uint64_t Size =
          std::max(Memory.Expected.size(), Memory.Output.size());
      if (!Size || Memory.Offset > UINT64_MAX - Size ||
          (Memory.PointerOffset && *Memory.PointerOffset > UINT64_MAX - 8))
        return failure(DeviceOptions);
      for (const auto *Data : {&Memory.Expected, &Memory.Output}) {
        if (Data->size() > FileByteLimit - Bytes)
          return failure(FileOptionsLimit);
        Bytes += Data->size();
      }
    }
  }
  return llvm::Error::success();
}

bool LinuxFiles::isDevice(uint32_t FD) const {
  auto I = Descriptors.find(FD);
  return I != Descriptors.end() &&
         std::holds_alternative<OpenDevice>(I->second);
}

std::variant<const LinuxDeviceOptions *, uint64_t>
LinuxFiles::mappingDevice(uint32_t FD) const {
  auto I = Descriptors.find(FD);
  if (I == Descriptors.end())
    return uint64_t(0) - BadDescriptor;
  if (const auto *Device = std::get_if<OpenDevice>(&I->second))
    return Device->Options;
  return static_cast<const LinuxDeviceOptions *>(nullptr);
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::deviceIoctl(uint32_t FD, uint32_t Request, uint64_t Argument,
                        ProcessResult &Result) {
  auto I = Descriptors.find(FD);
  if (I == Descriptors.end())
    return std::optional<uint64_t>(uint64_t(0) - BadDescriptor);
  auto *Device = std::get_if<OpenDevice>(&I->second);
  if (!Device)
    return unsupported(Result, DeviceIoctlMissing);
  const auto &Calls = Device->Options->Ioctls;
  const size_t Index = Device->NextIoctl;
  if (Index == Calls.size() || Calls[Index].Request != Request ||
      (Calls[Index].Argument && *Calls[Index].Argument != Argument)) {
    unsupported(Result, DeviceIoctlMissing);
    Result.Diagnostic +=
        llvm::formatv(" (transaction {0}, request {1:x})", Index, Request)
            .str();
    return std::optional<uint64_t>();
  }
  const auto &Call = Calls[Index];
  // Resolve every pointer and compare every input against the state at entry.
  // Validate all output spans before the first write or sequence consumption.
  // A contract mismatch is not an invented driver EFAULT/ENOTTY response.
  auto Accessible = [&](uint64_t Address, uint64_t Size,
                        uint8_t Permission) -> llvm::Expected<bool> {
    if (!Size)
      return true;
    if (Address > Layout.UserLimit || Size > Layout.UserLimit - Address)
      return false;
    return CPU.canAccess(Address, Size, Permission | UserAccessible);
  };
  struct WritePlan {
    uint64_t Address;
    llvm::ArrayRef<uint8_t> Bytes;
  };
  std::vector<WritePlan> Writes;
  for (const auto &Memory : Call.Memory) {
    uint64_t Base = Argument;
    if (Memory.PointerOffset) {
      if (*Memory.PointerOffset > UINT64_MAX - Base)
        return unsupported(Result, DeviceIoctlMemory);
      auto Access = Accessible(Base + *Memory.PointerOffset, 8, Read);
      if (!Access)
        return Access.takeError();
      if (!*Access)
        return unsupported(Result, DeviceIoctlMemory);
      auto Pointer = CPU.readInteger(Base + *Memory.PointerOffset, 8);
      if (!Pointer)
        return Pointer.takeError();
      Base = *Pointer;
    }
    if (Memory.Offset > UINT64_MAX - Base)
      return unsupported(Result, DeviceIoctlMemory);
    const uint64_t Address = Base + Memory.Offset;
    auto Readable = Accessible(Address, Memory.Expected.size(), Read);
    if (!Readable)
      return Readable.takeError();
    auto Writable = Accessible(Address, Memory.Output.size(), Write);
    if (!Writable)
      return Writable.takeError();
    if (!*Readable || !*Writable)
      return unsupported(Result, DeviceIoctlMemory);
    if (!Memory.Expected.empty()) {
      std::vector<uint8_t> Input(Memory.Expected.size());
      if (auto E = CPU.read(Address, Input))
        return std::move(E);
      if (Input != Memory.Expected)
        return unsupported(Result, DeviceIoctlMemory);
    }
    if (!Memory.Output.empty())
      Writes.push_back({Address, Memory.Output});
  }
  for (const auto &Plan : Writes)
    if (auto E = CPU.write(Plan.Address, Plan.Bytes))
      return std::move(E);
  ++Device->NextIoctl;
  return std::optional<uint64_t>(uint64_t(Call.Result));
}
} // namespace neverd::emulation::linux_model
