//===- KernelMMIO.cpp - Explicit registers and MMIO alias lifetimes -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Map only declared translated resources. Registers remain in one physical
/// bank through mapping changes; holes and undeclared access semantics fail.
///
//===----------------------------------------------------------------------===//

#include "KernelMMIO.h"

#include "neverd/emulation/DriverProfile.h"

#include <algorithm>
#include <limits>

namespace neverd::emulation {
namespace {
#define NEVERD_MMIO_TRANSACTION_TEXT(Name, Text) constexpr char Name[] = Text;
#include "KernelMMIOTransaction.def"
#undef NEVERD_MMIO_TRANSACTION_TEXT
llvm::Error mmioError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "MMIO: " + Message);
}
} // namespace

llvm::Error KernelMMIO::configure(uint64_t PDO) {
  const auto *Assignment = Resources.find(PDO);
  if (!Assignment || Assignment->Memory.empty())
    return llvm::Error::success();
  if (Devices.count(PDO))
    return mmioError("duplicate physical register bank");
  Devices.emplace(PDO, Device{Assignment->Memory, Assignment->PowerGeneration});
  return llvm::Error::success();
}

llvm::Error KernelMMIO::canRemove(uint64_t PDO) const {
  for (const auto &[Address, Mapping] : Mappings) {
    (void)Address;
    if (Mapping.PDO == PDO)
      return mmioError("device still owns an I/O-space mapping");
  }
  return llvm::Error::success();
}

llvm::Expected<uint64_t> KernelMMIO::map(uint64_t Physical, uint64_t Length,
                                         uint32_t Attributes, bool Extended) {
  const bool ReadOnly =
      Extended && Attributes == (mmio::PageReadOnly | mmio::PageNoCache);
  if (Extended
          ? !ReadOnly && Attributes != (mmio::PageReadWrite | mmio::PageNoCache)
          : Attributes != mmio::NonCached)
    return mmioError("mapping requires noncached non-executable RO or RW");
  if (!Length || Length > UINT64_MAX - Physical)
    return mmioError("invalid or overflowing physical mapping");
  uint64_t Owner = 0;
  size_t Index = 0;
  for (const auto &[PDO, Device] : Devices)
    for (size_t I = 0; I < Device.Resources.size(); ++I) {
      const auto &Resource = Device.Resources[I];
      if (Physical < Resource.TranslatedStart ||
          Physical - Resource.TranslatedStart >= Resource.Length ||
          Length > Resource.Length - (Physical - Resource.TranslatedStart))
        continue;
      if (Owner)
        return mmioError("physical range has multiple resource owners");
      Owner = PDO;
      Index = I;
    }
  if (!Owner)
    return mmioError("physical range is not one declared translated resource");
  const auto &Device = *Resources.find(Owner);
  if (!Device.Assigned || !Device.Present)
    return mmioError("resource is not assigned or hardware is absent");
  const uint64_t Offset = Physical & (profile::PageSize - 1);
  const uint64_t Size =
      (Length + Offset + profile::PageSize - 1) & ~(profile::PageSize - 1);
  const uint64_t Base = NextMapping ? NextMapping : profile::MMIOBase;
  const uint64_t End = profile::MMIOBase + profile::MMIOSize;
  if (Mappings.size() >= mmio::MaxMappings || Base >= End || Size > End - Base)
    return 0;
  const uint64_t Address = Base + Offset;
  GuestMMIOCallbacks Callbacks;
  const std::weak_ptr<unsigned char> OwnerLifetime = Lifetime;
  Callbacks.Validate = [this, Address, OwnerLifetime](
                           uint64_t Offset, uint64_t Size, bool Write) {
    if (OwnerLifetime.expired())
      return mmioError(ExpiredOwner);
    return validate(Address, Offset, Size, Write);
  };
  Callbacks.Read = [this, Address,
                    OwnerLifetime](uint64_t Offset,
                                   unsigned Size) -> llvm::Expected<uint64_t> {
    if (OwnerLifetime.expired())
      return mmioError(ExpiredOwner);
    return read(Address, Offset, Size);
  };
  Callbacks.Write = [this, Address, OwnerLifetime](
                        uint64_t Offset, unsigned Size, uint64_t Value) {
    if (OwnerLifetime.expired())
      return mmioError(ExpiredOwner);
    return write(Address, Offset, Size, Value);
  };
  Callbacks.PrepareRead =
      [this, Address,
       OwnerLifetime](uint64_t Offset,
                      unsigned Size) -> llvm::Expected<GuestMMIOPreparedRead> {
    if (OwnerLifetime.expired())
      return mmioError(ExpiredOwner);
    auto Value = peek(Address, Offset, Size);
    if (!Value)
      return Value.takeError();
    return GuestMMIOPreparedRead{*Value,
                                 [this, Address, Offset, Size, OwnerLifetime,
                                  Expected = *Value]() -> llvm::Error {
                                   if (OwnerLifetime.expired())
                                     return mmioError(ExpiredOwner);
                                   auto Current = peek(Address, Offset, Size);
                                   if (!Current)
                                     return Current.takeError();
                                   if (*Current != Expected)
                                     return mmioError(ChangedRead);
                                   auto Committed = read(Address, Offset, Size);
                                   return Committed ? llvm::Error::success()
                                                    : Committed.takeError();
                                 }};
  };
  if (auto E = Memory.mapMMIO(Base, Size, std::move(Callbacks))) {
    bool Exhausted = false;
    auto Remaining = llvm::handleErrors(
        std::move(E), [&](const GuestMemoryLimitError &) { Exhausted = true; });
    if (Remaining)
      return Remaining;
    if (Exhausted)
      return 0;
  }
  Mappings.emplace(Address, Mapping{Owner, Index, Device.Epoch, Address, Length,
                                    Base, Size, Physical, ReadOnly});
  NextMapping = Base + Size;
  return Address;
}

llvm::Error KernelMMIO::unmap(uint64_t Address, uint64_t Length) {
  const auto It = Mappings.find(Address);
  if (It == Mappings.end() || It->second.Length != Length)
    return mmioError("unmap requires the original virtual base and byte count");
  const auto &Mapping = It->second;
  if (auto E = Memory.unmapMMIO(Mapping.PageBase, Mapping.PageSize))
    return E;
  Mappings.erase(It);
  return llvm::Error::success();
}

llvm::Error KernelMMIO::validate(uint64_t Address, uint64_t Offset,
                                 uint64_t Size, bool Write) const {
  const auto It = Mappings.find(Address);
  if (It == Mappings.end())
    return mmioError("access lost its live mapping");
  const auto &Mapping = It->second;
  const uint64_t Prefix = Mapping.Address - Mapping.PageBase;
  if (Offset < Prefix || Offset - Prefix >= Mapping.Length ||
      Size > Mapping.Length - (Offset - Prefix))
    return mmioError("access exceeds the exact mapped physical range");
  const auto &Device = *Resources.find(Mapping.PDO);
  if (!Device.Present || !Device.Assigned || Device.Epoch != Mapping.Epoch)
    return mmioError("access targets an unavailable physical resource epoch");
  if (Device.Power != DevicePowerState::D0)
    return mmioError("register access requires physical device power D0");
  const auto &Resource =
      Devices.at(Mapping.PDO).Resources[Mapping.ResourceIndex];
  const uint64_t RegisterOffset =
      Mapping.Physical - Resource.TranslatedStart + Offset - Prefix;
  for (const auto &Register : Resource.Registers) {
    if (Register.Offset != RegisterOffset || Register.Width != Size)
      continue;
    if (Write &&
        (Mapping.ReadOnly || Register.Access == DriverRegisterAccess::ReadOnly))
      return mmioError("write to a read-only register or mapping");
    return llvm::Error::success();
  }
  return mmioError("access requires an exact declared register and width");
}

void KernelMMIO::restorePowerContext(uint64_t PDO) {
  auto &Device = Devices.at(PDO);
  const auto &Assignment = *Resources.find(PDO);
  if (Device.PowerGeneration == Assignment.PowerGeneration)
    return;
  Device.Resources = Assignment.Memory;
  Device.PowerGeneration = Assignment.PowerGeneration;
}

llvm::Expected<uint64_t> KernelMMIO::peek(uint64_t Address, uint64_t Offset,
                                          unsigned Size) const {
  if (auto E = validate(Address, Offset, Size, false))
    return E;
  const auto &Mapping = Mappings.at(Address);
  const auto &Device = Devices.at(Mapping.PDO);
  const auto &Assignment = *Resources.find(Mapping.PDO);
  // Preview the same reset value without changing the register bank's power
  // generation. Only committing the read may restore that context.
  const auto &Resource = (Device.PowerGeneration == Assignment.PowerGeneration
                              ? Device.Resources
                              : Assignment.Memory)[Mapping.ResourceIndex];
  const uint64_t RegisterOffset = Mapping.Physical - Resource.TranslatedStart +
                                  Offset - (Address - Mapping.PageBase);
  for (const auto &Register : Resource.Registers)
    if (Register.Offset == RegisterOffset)
      return Register.Value;
  llvm_unreachable(MissingRegister);
}

llvm::Expected<uint64_t> KernelMMIO::read(uint64_t Address, uint64_t Offset,
                                          unsigned Size) {
  auto Value = peek(Address, Offset, Size);
  if (!Value)
    return Value.takeError();
  restorePowerContext(Mappings.at(Address).PDO);
  return *Value;
}

llvm::Error KernelMMIO::write(uint64_t Address, uint64_t Offset, unsigned Size,
                              uint64_t Value) {
  if (auto E = validate(Address, Offset, Size, true))
    return E;
  const auto &Mapping = Mappings.at(Address);
  restorePowerContext(Mapping.PDO);
  auto &Resource = Devices.at(Mapping.PDO).Resources[Mapping.ResourceIndex];
  const uint64_t RegisterOffset = Mapping.Physical - Resource.TranslatedStart +
                                  Offset - (Address - Mapping.PageBase);
  for (auto &Register : Resource.Registers)
    if (Register.Offset == RegisterOffset) {
      Register.Value =
          uint32_t(Value) & uint32_t((uint64_t(1) << (Size * 8)) - 1);
      return llvm::Error::success();
    }
  llvm_unreachable(MissingRegister);
}

} // namespace neverd::emulation
