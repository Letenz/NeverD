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
#include <climits>
#include <limits>
#include <utility>

namespace neverd::emulation {
namespace {
#define NEVERD_MMIO_TRANSACTION_TEXT(Name, Text) constexpr char Name[] = Text;
#include "KernelMMIOTransaction.def"
#undef NEVERD_MMIO_TRANSACTION_TEXT
llvm::Error mmioError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "MMIO: " + Message);
}
template <typename State, typename F>
auto withOwner(const std::shared_ptr<State> &S, F Call)
    -> decltype(Call(*S->Owner)) {
  std::lock_guard Lock(*S->Mutex);
  if (!S->Owner)
    return mmioError(ExpiredOwner);
  return Call(*S->Owner);
}
} // namespace

KernelMMIO::KernelMMIO(GuestMemory &Memory, const KernelResources &Resources)
    : Memory(Memory), Resources(Resources),
      State(std::make_shared<CallbackState>(
          CallbackState{Resources.transactionMutex(), this})) {}
KernelMMIO::~KernelMMIO() {
  // A callback either finishes while the owner still exists, or observes its
  // retirement. Locking an unrelated weak token cannot provide this barrier.
  std::lock_guard Lock(*State->Mutex);
  State->Owner = nullptr;
}

GuestMMIOCallbacks KernelMMIO::callbacks(uint64_t Address) {
  GuestMMIOCallbacks Callbacks;
  const auto S = State;
  Callbacks.Validate = [S, Address](uint64_t Offset, uint64_t Size,
                                    bool Write) {
    return withOwner(S, [&](KernelMMIO &Owner) {
      return Owner.validate(Address, Offset, Size, Write);
    });
  };
  Callbacks.Read = [S, Address](uint64_t Offset, unsigned Size) {
    return withOwner(S, [&](KernelMMIO &Owner) {
      return Owner.read(Address, Offset, Size);
    });
  };
  Callbacks.Write = [S, Address](uint64_t Offset, unsigned Size,
                                 uint64_t Value) {
    return withOwner(S, [&](KernelMMIO &Owner) {
      return Owner.write(Address, Offset, Size, Value);
    });
  };
  Callbacks.PrepareAtomic = [S, Address](uint64_t Offset, unsigned Size) {
    return withOwner(S, [&](KernelMMIO &Owner) {
      return Owner.prepare(Address, Offset, Size, true);
    });
  };
  Callbacks.PrepareRead =
      [S, Address](uint64_t Offset,
                   unsigned Size) -> llvm::Expected<GuestMMIOPreparedRead> {
    auto Preview = withOwner(S, [&](KernelMMIO &Owner) {
      return Owner.prepare(Address, Offset, Size, false);
    });
    if (!Preview)
      return Preview.takeError();
    uint64_t Value = 0;
    for (unsigned N = 0; N < Size; ++N)
      Value |= uint64_t(Preview->Value[N]) << (N * CHAR_BIT);
    return GuestMMIOPreparedRead{Value, [Preview = std::move(*Preview)] {
                                   return Preview.Commit(Preview.Value);
                                 }};
  };
  return Callbacks;
}

llvm::Expected<GuestMMIOPreparedAtomic> KernelMMIO::prepare(uint64_t Address,
                                                            uint64_t Offset,
                                                            unsigned Size,
                                                            bool Write) {
  if (auto E = validate(Address, Offset, Size, Write))
    return E;
  if (!Size || Size > sizeof(uint32_t))
    return mmioError(AtomicWidth);
  auto Value = peek(Address, Offset, Size);
  if (!Value)
    return Value.takeError();
  const auto &Mapping = Mappings.at(Address);
  const auto Revision = Devices.at(Mapping.PDO).Revision;
  const auto ResourceRevision = Resources.find(Mapping.PDO)->Revision;
  // Shared identity cannot wrap or be recreated by restoring the same value.
  // A shared consumed bit also protects copied provider commit callbacks.
  auto Consumed = std::make_shared<bool>(false);
  std::vector<uint8_t> Bytes(Size);
  for (unsigned N = 0; N < Size; ++N)
    Bytes[N] = uint8_t(*Value >> (N * CHAR_BIT));
  return GuestMMIOPreparedAtomic{
      std::move(Bytes),
      [S = State, Address, Offset, Size, Write, Revision, ResourceRevision,
       Consumed](llvm::ArrayRef<uint8_t> Bytes) -> llvm::Error {
        return withOwner(S, [&](KernelMMIO &Owner) -> llvm::Error {
          if (std::exchange(*Consumed, true))
            return mmioError(ChangedRead);
          if (Bytes.size() != Size)
            return mmioError(AtomicWidth);
          if (auto E = Owner.validate(Address, Offset, Size, Write))
            return E;
          const auto PDO = Owner.Mappings.at(Address).PDO;
          if (Owner.Devices.at(PDO).Revision != Revision ||
              Owner.Resources.find(PDO)->Revision != ResourceRevision)
            return mmioError(ChangedRead);
          if (!Write) {
            auto Result = Owner.read(Address, Offset, Size);
            return Result ? llvm::Error::success() : Result.takeError();
          }
          uint64_t Value = 0;
          for (unsigned N = 0; N < Size; ++N)
            Value |= uint64_t(Bytes[N]) << (N * CHAR_BIT);
          return Owner.write(Address, Offset, Size, Value);
        });
      }};
}

llvm::Error KernelMMIO::configure(uint64_t PDO) {
  std::lock_guard Lock(*State->Mutex);
  const auto *Assignment = Resources.find(PDO);
  if (!Assignment || Assignment->Memory.empty())
    return llvm::Error::success();
  if (Devices.count(PDO))
    return mmioError("duplicate physical register bank");
  Devices.emplace(PDO, Device{Assignment->Memory, Assignment->PowerGeneration});
  return llvm::Error::success();
}

llvm::Error KernelMMIO::canRemove(uint64_t PDO) const {
  std::lock_guard Lock(*State->Mutex);
  for (const auto &[Address, Mapping] : Mappings) {
    (void)Address;
    if (Mapping.PDO == PDO)
      return mmioError("device still owns an I/O-space mapping");
  }
  return llvm::Error::success();
}

llvm::Expected<uint64_t> KernelMMIO::map(uint64_t Physical, uint64_t Length,
                                         uint32_t Attributes, bool Extended) {
  std::lock_guard Lock(*State->Mutex);
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
  auto Callbacks = callbacks(Address);
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
  std::lock_guard Lock(*State->Mutex);
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
  auto Next = Assignment.Memory;
  auto Revision = std::make_shared<unsigned char>(0);
  Device.Resources = std::move(Next);
  Device.PowerGeneration = Assignment.PowerGeneration;
  Device.Revision = std::move(Revision);
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
  auto Revision = std::make_shared<unsigned char>(0);
  restorePowerContext(Mapping.PDO);
  auto &Resource = Devices.at(Mapping.PDO).Resources[Mapping.ResourceIndex];
  const uint64_t RegisterOffset = Mapping.Physical - Resource.TranslatedStart +
                                  Offset - (Address - Mapping.PageBase);
  for (auto &Register : Resource.Registers)
    if (Register.Offset == RegisterOffset) {
      Register.Value =
          uint32_t(Value) & uint32_t((uint64_t(1) << (Size * 8)) - 1);
      Devices.at(Mapping.PDO).Revision = std::move(Revision);
      return llvm::Error::success();
    }
  llvm_unreachable(MissingRegister);
}

} // namespace neverd::emulation
