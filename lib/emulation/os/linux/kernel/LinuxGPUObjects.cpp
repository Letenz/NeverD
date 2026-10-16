//===- LinuxGPUObjects.cpp - Owned coherent KGSL memory objects
//------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded LP64 GPU memory allocation, query, mapping and release. Device
/// addresses identify retained RAM, never a host driver or guessed contents.
//===----------------------------------------------------------------------===//
#include "LinuxFiles.h"
#include "LinuxKGSL.h"
#include "LinuxMemory.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation::linux_model {
llvm::Expected<MemoryView> LinuxMemory::allocateGPUObject(uint64_t Size) {
  const uint64_t Used = Space.physicalMemory()->allocatedBytes();
  if (Used > Limit || Size > Limit - Used)
    return llvm::make_error<GuestMemoryLimitError>();
  return Space.allocateBacking(Size);
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::gpuObjectIoctl(LinuxDeviceState &Device, uint32_t Request,
                           uint64_t Argument, ProcessResult &Result) {
  using Outcome = std::optional<uint64_t>;
  if (!Memory || !Device.GPU)
    return unsupported(Result,
                       "GPU objects require owned RAM and process state");
  const bool LegacyAlloc = Request == kgsl::AllocateID;
  const bool LegacyInfo = Request == kgsl::MemoryInfo;
  const bool LegacyFree = Request == kgsl::FreeID;
  const unsigned Size = Request == kgsl::FreeObject                 ? 32
                        : Request == kgsl::FreeMemory || LegacyFree ? 8
                        : LegacyInfo                                ? 72
                                                                    : 48;
  if (Argument > Layout.UserLimit || Size > Layout.UserLimit - Argument)
    return Outcome(uint64_t(0) - BadAddress);
  auto Readable = CPU.canAccess(Argument, Size, Read | UserAccessible);
  if (!Readable)
    return Readable.takeError();
  if (!*Readable)
    return Outcome(uint64_t(0) - BadAddress);
  uint8_t Data[72]{};
  if (auto E = CPU.read(Argument, llvm::MutableArrayRef(Data, Size)))
    return std::move(E);
  auto Get32 = [&](unsigned At) {
    return llvm::support::endian::read32le(Data + At);
  };
  auto Get64 = [&](unsigned At) {
    return llvm::support::endian::read64le(Data + At);
  };
  auto Put64 = [&](unsigned At, uint64_t Value) {
    llvm::support::endian::write64le(Data + At, Value);
  };
  auto Put32 = [&](unsigned At, uint32_t Value) {
    llvm::support::endian::write32le(Data + At, Value);
  };
  // Preflight the complete _IOWR record before changing allocation state.
  auto Writable = [&]() {
    return CPU.canAccess(Argument, Size, Write | UserAccessible);
  };
  auto WriteRecord = [&]() {
    return CPU.write(Argument, llvm::ArrayRef(Data, Size));
  };
  auto &GPU = *Device.GPU;
  if (Request == kgsl::FreeObject || Request == kgsl::FreeMemory ||
      LegacyFree) {
    auto I = GPU.Allocations.end();
    const uint32_t ID = Get32(LegacyFree ? 0 : 16);
    if (Request == kgsl::FreeMemory)
      I = GPU.Allocations.find(Get64(0));
    else if (ID)
      I = std::find_if(GPU.Allocations.begin(), GPU.Allocations.end(),
                       [&](const auto &A) { return A.second.ID == ID; });
    if (I == GPU.Allocations.end())
      return Outcome(uint64_t(0) - InvalidArgument);
    if (Request == kgsl::FreeObject && Get64(0))
      return unsupported(Result, "GPU asynchronous free flags are unmodeled");
    if (I->second.PendingFree)
      return Outcome(uint64_t(0) - 16); // EBUSY: a VMA still holds this object.
    if (LegacyFree) {
      auto Allowed = Writable();
      if (!Allowed)
        return Allowed.takeError();
      if (!*Allowed)
        return unsupported(Result, "GPU object result is not fully writable");
      if (auto E = WriteRecord())
        return std::move(E);
    }
    if (I->second.UserAddress)
      I->second.PendingFree = true;
    else
      GPU.Allocations.erase(I);
    return Outcome(0);
  }
  if (Request == kgsl::ObjectInfo || LegacyInfo) {
    const uint32_t ID = Get32(LegacyInfo ? 8 : 40);
    auto I = GPU.Allocations.end();
    if (ID)
      I = std::find_if(GPU.Allocations.begin(), GPU.Allocations.end(),
                       [&](const auto &A) { return A.second.ID == ID; });
    else if (LegacyInfo && Get64(0)) {
      I = GPU.Allocations.upper_bound(Get64(0));
      if (I != GPU.Allocations.begin()) {
        --I;
        if (Get64(0) - I->first >= I->second.Bytes.size())
          I = GPU.Allocations.end();
      } else
        I = GPU.Allocations.end();
    }
    if (I == GPU.Allocations.end())
      return Outcome(uint64_t(0) - InvalidArgument);
    if (!I->second.ID)
      return unsupported(Result, "GPU imported allocation IDs are unmodeled");
    auto Allowed = Writable();
    if (!Allowed)
      return Allowed.takeError();
    if (!*Allowed)
      return unsupported(Result, "GPU object result is not fully writable");
    const auto &Object = I->second;
    Put64(0, I->first);
    if (LegacyInfo) {
      Put32(8, Object.ID);
      Put32(12, uint32_t(Object.Flags));
    } else
      Put64(8, Object.Flags);
    Put64(16, Object.Bytes.size());
    Put64(24, Object.Bytes.size() + 4096); // IOMMU guard footprint.
    Put64(32, Object.UserAddress);
    if (auto E = WriteRecord())
      return std::move(E);
    return Outcome(0);
  }
  const uint64_t Length = Get64(LegacyAlloc ? 8 : 0);
  uint64_t Flags =
      LegacyAlloc ? uint64_t(Get32(4)) | kgsl::Force32Bit : Get64(8);
  if (!Length || Length > UINT32_MAX)
    return Outcome(uint64_t(0) - InvalidArgument);
  if ((Flags & ~(kgsl::GPUReadOnly | kgsl::Force32Bit | kgsl::AlignmentMask)) ||
      (!LegacyAlloc && Get32(36)))
    return unsupported(Result,
                       "GPU allocation flags or metadata are unmodeled");
  const uint64_t Rounded = (Length + 4095) & ~uint64_t(4095);
  // The admitted 4 KiB page profile normalizes hints to [4 KiB, 32 MiB].
  const uint64_t Align = std::clamp((Flags & kgsl::AlignmentMask) >> 16,
                                    uint64_t(12), uint64_t(25));
  Flags = (Flags & ~kgsl::AlignmentMask) | (Align << 16);
  const auto GPUAddress = kgsl::allocationAddress(
      *Device.Options, GPU.NextAddress, Rounded, 1ULL << Align);
  if (Rounded > kgsl::MaximumAllocation ||
      GPU.Allocations.size() >= kgsl::MaximumAllocations || !GPUAddress)
    return unsupported(Result, "GPU object memory budget exhausted");
  // IDs are process-scoped. Avoid the explicitly supplied legacy mapping
  // offsets so every returned ID can also be used as an mmap page offset.
  uint32_t ID = 1;
  for (;; ++ID) {
    bool Occupied = false;
    for (const auto &[Address, Object] : GPU.Allocations)
      Occupied |= Object.ID == ID;
    for (const auto &Mapping : Device.Options->ReadOnlyMappings)
      Occupied |= uint64_t(ID) * 4096 >= Mapping.Offset &&
                  uint64_t(ID) * 4096 - Mapping.Offset < Mapping.Bytes.size();
    if (!Occupied)
      break;
  }
  auto Allowed = Writable();
  if (!Allowed)
    return Allowed.takeError();
  if (!*Allowed)
    return unsupported(Result, "GPU object result is not fully writable");
  auto Bytes = Memory->allocateGPUObject(Rounded);
  if (!Bytes) {
    auto E = Bytes.takeError();
    if (!E.isA<GuestMemoryLimitError>())
      return std::move(E);
    llvm::consumeError(std::move(E));
    return Outcome(uint64_t(0) - NoMemory);
  }
  if (LegacyAlloc) {
    Put32(0, ID);
    Put32(4, uint32_t(Flags));
    Put64(8, Rounded);
    Put64(16, Rounded + 4096);
    Put64(24, *GPUAddress);
  } else {
    Put64(0, Rounded);
    Put64(8, Flags);
    Put64(24, Rounded + 4096);
    Put32(32, ID);
  }
  if (auto E = WriteRecord())
    return std::move(E);
  GPU.Allocations.emplace(*GPUAddress,
                          GPUAllocation{std::move(*Bytes), Flags, ID});
  GPU.NextAddress = *GPUAddress + Rounded + 4096;
  return Outcome(0);
}

bool LinuxMemory::overlapsGPUObject(uint64_t Address, uint64_t Size,
                                    bool PartialOnly) const {
  if (!Size || Address > UINT64_MAX - Size)
    return false;
  for (const auto &Alias : GPUObjectRanges)
    if (Address < Alias.Begin + Alias.Size && Alias.Begin < Address + Size &&
        (!PartialOnly || Address > Alias.Begin ||
         Address + Size < Alias.Begin + Alias.Size))
      return true;
  return false;
}

void LinuxMemory::retireGPUObjects(uint64_t Address, uint64_t Size) {
  std::erase_if(GPUObjectRanges, [&](const GPUAlias &Alias) {
    if (Address > Alias.Begin || Address + Size < Alias.Begin + Alias.Size)
      return false;
    auto &Objects = Alias.Owner->GPU->Allocations;
    auto I = Objects.find(Alias.GPUAddress);
    if (I != Objects.end()) {
      I->second.UserAddress = 0;
      if (I->second.PendingFree)
        Objects.erase(I);
    }
    return true;
  });
}

llvm::Expected<std::optional<uint64_t>>
LinuxMemory::mapGPUObject(const ProcessServiceEvent &Event,
                          std::shared_ptr<LinuxDeviceState> Owner,
                          ProcessResult &Result) {
  using Outcome = std::optional<uint64_t>;
  auto Unsupported = [&](const char *Message) -> Outcome {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = Message;
    return std::nullopt;
  };
  const auto [Hint, Length, Protection, Flags, FD, Offset] = Event.Arguments;
  auto &Objects = Owner->GPU->Allocations;
  auto I = std::find_if(Objects.begin(), Objects.end(), [&](const auto &A) {
    return A.second.ID && uint64_t(A.second.ID) == Offset / PageSize;
  });
  if (I == Objects.end())
    I = Objects.find(Offset);
  if (I == Objects.end()) {
    auto Interior = Objects.upper_bound(Offset);
    if (Interior != Objects.begin()) {
      --Interior;
      if (Offset - Interior->first < Interior->second.Bytes.size())
        return Unsupported("GPU interior-address mmap offsets are unmodeled");
    }
  }
  if (I == Objects.end() || !I->second.ID)
    return Outcome(uint64_t(0) - InvalidArgument);
  auto &Object = I->second;
  if (Object.UserAddress)
    return Outcome(uint64_t(0) - 16);
  const uint64_t Size = *roundSize(Length);
  if (Size == Object.Bytes.size() + PageSize)
    return Unsupported("GPU guard-page fault delivery is unmodeled");
  if (Size != Object.Bytes.size())
    return Outcome(uint64_t(0) - 34); // ERANGE
  if (Flags != MapShared || (Protection & ~(ProtRead | ProtWrite)) ||
      (Protection && !(Protection & ProtRead)))
    return Unsupported(
        "GPU objects require whole coherent shared RAM mappings");
  if (Owner->Options->AccessMode == 1 ||
      ((Protection & ProtWrite) && Owner->Options->AccessMode != 2))
    return Outcome(uint64_t(0) - AccessDenied);
  auto Address = placement(Hint, Size);
  if (!Address)
    return Address.takeError();
  if (!*Address || Space.mappedBytes() > Limit ||
      Size > Limit - Space.mappedBytes())
    return Outcome(uint64_t(0) - NoMemory);
  // Allocated objects have one retained region, independent of CPU mappings.
  const auto &Slice = Object.Bytes.slices().front();
  if (auto E = Space.mapRegion(**Address, Slice.Region, Slice.Offset, Size,
                               permissions(Protection)))
    return std::move(E);
  Object.UserAddress = **Address;
  GPUObjectRanges.push_back(
      {**Address, Size, I->first, Offset, std::move(Owner)});
  return Outcome(**Address);
}
} // namespace neverd::emulation::linux_model
