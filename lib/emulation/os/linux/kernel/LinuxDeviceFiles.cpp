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
#include "LinuxKGSL.h"

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
  if (Options.Binder && (Options.KGSL || !Options.Ioctls.empty() ||
                         !Options.ReadOnlyMappings.empty()))
    return failure(DeviceOptions);
  if (Options.KGSL) {
    const auto *Mapping = kgsl::memstore(Options);
    if (!Mapping || !Options.KGSL->ContextLimit ||
        Options.KGSL->ContextLimit > kgsl::MaximumContexts ||
        (uint64_t(Options.KGSL->ContextLimit) + 1) * kgsl::SlotBytes >
            Mapping->Bytes.size())
      return failure(DeviceOptions);
    if (Options.KGSL->MemoryObjects && Options.AccessMode != 2)
      return failure(DeviceOptions);
    if (Options.KGSL->SynchronousA3xx && !Mapping->Updates.empty())
      return failure(DeviceOptions);
  }
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
    if (Mapping.Updates.size() > DeviceIoctlLimit - Transactions)
      return failure(DeviceOptions);
    Transactions += Mapping.Updates.size();
    uint64_t Previous = 0;
    for (const auto &Update : Mapping.Updates) {
      if (Update.AfterNanoseconds < Previous || Update.Bytes.empty() ||
          Update.Bytes.size() > DeviceMappingPageSize || Update.Offset > Size ||
          Update.Bytes.size() > Size - Update.Offset)
        return failure(DeviceOptions);
      if (Update.Bytes.size() > FileByteLimit - Bytes)
        return failure(FileOptionsLimit);
      Bytes += Update.Bytes.size();
      Previous = Update.AfterNanoseconds;
    }
    Ranges.emplace_back(Mapping.Offset, Mapping.Offset + Size);
  }
  std::sort(Ranges.begin(), Ranges.end());
  for (size_t I = 1; I < Ranges.size(); ++I)
    if (Ranges[I].first < Ranges[I - 1].second)
      return failure(DeviceOptions);
  for (const auto &Call : Options.Ioctls) {
    if (Options.KGSL &&
        (kgsl::isContextRequest(Call.Request) ||
         (Options.KGSL->SynchronousA3xx &&
          kgsl::isCommandRequest(Call.Request)) ||
         (Options.KGSL->MemoryObjects && (kgsl::isObjectRequest(Call.Request) ||
                                          Call.Request == kgsl::FreeMemory))))
      return failure(DeviceOptions);
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
         std::holds_alternative<OpenDevice>(*I->second);
}

std::variant<std::shared_ptr<LinuxDeviceState>, uint64_t>
LinuxFiles::mappingDevice(uint32_t FD) const {
  auto I = Descriptors.find(FD);
  if (I == Descriptors.end())
    return uint64_t(0) - BadDescriptor;
  if (const auto *Device = std::get_if<OpenDevice>(I->second.get()))
    return *Device;
  return std::shared_ptr<LinuxDeviceState>();
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::deviceIoctl(uint32_t FD, uint32_t Request, uint64_t Argument,
                        ProcessResult &Result) {
  auto I = Descriptors.find(FD);
  if (I == Descriptors.end())
    return std::optional<uint64_t>(uint64_t(0) - BadDescriptor);
  auto *Description = std::get_if<OpenDevice>(I->second.get());
  if (!Description)
    return unsupported(Result, DeviceIoctlMissing);
  auto *Device = Description->get();
  if (Device->Options->Binder)
    return binderIoctl(*Device, Request, Argument, Result);
  if (Device->Options->KGSL && kgsl::isContextRequest(Request))
    return contextIoctl(*Device, Request, Argument, Result);
  if (Device->Options->KGSL && Device->Options->KGSL->MemoryObjects &&
      (kgsl::isObjectRequest(Request) || Request == kgsl::FreeMemory))
    return gpuObjectIoctl(*Device, Request, Argument, Result);
  if (Device->Options->KGSL && Device->Options->KGSL->SynchronousA3xx &&
      kgsl::isCommandRequest(Request))
    return gpuIoctl(*Device, Request, Argument, Result);
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
