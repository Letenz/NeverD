//===- LinuxKGSL.cpp - Workload-owned idle GPU contexts and timestamps ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Implements a declared LP64 context subset. No GPU command or vendor SDK is
/// inferred. Shared bytes and optional producer inputs belong to LinuxMemory.
//===----------------------------------------------------------------------===//
#include "LinuxKGSL.h"

#include "LinuxFiles.h"
#include "LinuxMemory.h"

#include "llvm/Support/Endian.h"

#include <cassert>

namespace neverd::emulation::linux_model {
llvm::Expected<std::optional<uint64_t>>
LinuxFiles::contextIoctl(LinuxDeviceState &Device, uint32_t Request,
                         uint64_t Argument, ProcessResult &Result) {
  using Reply = std::optional<uint64_t>;
  if (!Memory)
    return unsupported(Result,
                       "GPU context model requires shared device memory");
  const auto &Options = *Device.Options;
  const auto *Mapping = kgsl::memstore(Options);
  assert(Options.KGSL && Mapping && "context options must be validated");
  const unsigned Size = Request == kgsl::Create    ? 8
                        : Request == kgsl::Destroy ? 4
                                                   : 12;
  if (Argument > Layout.UserLimit || Size > Layout.UserLimit - Argument)
    return Reply(uint64_t(0) - BadAddress);
  auto Access = CPU.canAccess(Argument, Size, Read | UserAccessible);
  if (!Access)
    return Access.takeError();
  if (!*Access)
    return Reply(uint64_t(0) - BadAddress);
  uint8_t Data[12]{};
  if (auto E = CPU.read(Argument, llvm::MutableArrayRef(Data, Size)))
    return std::move(E);
  uint32_t ID = llvm::support::endian::read32le(Data);
  if (Request == kgsl::Create) {
    if ((ID & kgsl::CreateFlags) != kgsl::CreateFlags)
      return Reply(uint64_t(0) - InvalidArgument);
    if (ID != kgsl::CreateFlags && ID != kgsl::ReturnedFlags)
      return unsupported(
          Result, "GPU context creation flags are outside the idle model");
  }
  if (Request != kgsl::Create && !Device.Contexts.contains(ID))
    return Reply(uint64_t(0) - InvalidArgument);
  if (Request == kgsl::Destroy) {
    if (!Memory->contextIdle(Device, ID))
      return unsupported(Result, "GPU context retirement requires equal "
                                 "consumed and retired timestamps");
    Device.Contexts.erase(ID);
    Device.QueuedTimestamps.erase(ID);
    return Reply(0);
  }
  // Preflight copy-out before allocating a context or changing shared bytes.
  // Mixed/partial kernel copy outcomes are deliberately outside this subset.
  auto Writable = CPU.canAccess(Argument, Size, Write | UserAccessible);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return unsupported(Result,
                       "GPU context result buffer is not fully writable");
  if (Request == kgsl::Create) {
    std::set<uint32_t> Used;
    for (const auto &Description : DeviceDescriptions)
      if (auto Other = Description.lock())
        if (Other->Options == &Options)
          Used.insert(Other->Contexts.begin(), Other->Contexts.end());
    for (ID = 1; ID <= Options.KGSL->ContextLimit && Used.contains(ID); ++ID) {
    }
    if (ID > Options.KGSL->ContextLimit)
      return Reply(uint64_t(0) - 28); // ENOSPC: finite memstore context IDs.
    uint8_t Zero[4]{};
    if (auto E = Memory->writeDeviceBytes(*Mapping,
                                          uint64_t(ID) * kgsl::SlotBytes, Zero))
      return std::move(E);
    if (auto E = Memory->writeDeviceBytes(
            *Mapping, uint64_t(ID) * kgsl::SlotBytes + 8, Zero))
      return std::move(E);
    llvm::support::endian::write32le(Data + 4, ID);
    llvm::support::endian::write32le(Data, kgsl::ReturnedFlags);
    if (auto E = CPU.write(Argument, llvm::ArrayRef(Data, Size)))
      return std::move(E);
    Device.Contexts.insert(ID);
    Device.QueuedTimestamps.emplace(ID, 0);
    return Reply(0);
  }
  const uint32_t Type = llvm::support::endian::read32le(Data + 4);
  // The queued value is context-owned, not either word in the shared memstore.
  // Its initial zero or admitted command submissions prove the queued value
  // only when no declared external producer can affect this context's slot.
  if (Type == 3)
    for (const auto &Update : Mapping->Updates)
      if (Update.Offset < uint64_t(ID + 1) * kgsl::SlotBytes &&
          uint64_t(ID) * kgsl::SlotBytes < Update.Offset + Update.Bytes.size())
        return unsupported(
            Result, "GPU queued timestamp requires explicit submission state");
  const auto Bytes = Memory->deviceBytes(*Mapping);
  const uint64_t Offset = uint64_t(ID) * kgsl::SlotBytes + (Type == 2 ? 8 : 0);
  const uint32_t Timestamp =
      Type == 1 || Type == 2
          ? llvm::support::endian::read32le(Bytes.data() + Offset)
      : Type == 3 ? Device.QueuedTimestamps.at(ID)
                  : 0;
  llvm::support::endian::write32le(Data + 8, Timestamp);
  if (auto E = CPU.write(Argument, llvm::ArrayRef(Data, Size)))
    return std::move(E);
  return Reply(Type >= 1 && Type <= 3 ? 0 : uint64_t(0) - InvalidArgument);
}

bool LinuxMemory::contextIdle(const LinuxDeviceState &Device,
                              uint32_t ID) const {
  const auto *Mapping = kgsl::memstore(*Device.Options);
  if (!Mapping)
    return false;
  auto Bytes = deviceBytes(*Mapping);
  const auto *Slot = Bytes.data() + uint64_t(ID) * kgsl::SlotBytes;
  return llvm::support::endian::read32le(Slot) ==
         llvm::support::endian::read32le(Slot + 8);
}

bool LinuxFiles::canRelease(
    const std::shared_ptr<Descriptor> &Description) const {
  if (Description.use_count() != 1)
    return true;
  const auto *Device = std::get_if<OpenDevice>(Description.get());
  if (!Device || Device->use_count() != 1)
    return true;
  if ((*Device)->hasPendingBinderWork())
    return false;
  for (uint32_t ID : (*Device)->Contexts)
    if (!Memory || !Memory->contextIdle(**Device, ID))
      return false;
  return true;
}
} // namespace neverd::emulation::linux_model
